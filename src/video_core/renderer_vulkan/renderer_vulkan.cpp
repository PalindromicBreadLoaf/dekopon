// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/assert.h"
#include "common/logging/log.h"
#include "common/memory_detect.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "video_core/gpu.h"
#include "video_core/overlay.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"
#include "video_core/renderer_vulkan/vk_memory_util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

#include "video_core/host_shaders/vulkan_present_anaglyph_frag.h"
#include "video_core/host_shaders/vulkan_present_frag.h"
#include "video_core/host_shaders/vulkan_present_interlaced_frag.h"
#include "video_core/host_shaders/vulkan_present_vert.h"

#include "video_core/host_shaders/vulkan_cursor_frag.h"
#include "video_core/host_shaders/vulkan_cursor_vert.h"
#include "video_core/host_shaders/vulkan_overlay_frag.h"
#include "video_core/host_shaders/vulkan_overlay_vert.h"
#include "video_core/renderer_vulkan/overlay_font.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <vk_mem_alloc.h>
#if defined(__APPLE__) && !defined(HAVE_LIBRETRO)
#include "common/apple_utils.h"
#endif

#ifdef ENABLE_SDL2
#include <SDL.h>
#endif

MICROPROFILE_DEFINE(Vulkan_RenderFrame, "Vulkan", "Render Frame", MP_RGB(128, 128, 64));

namespace Vulkan {

struct ScreenRectVertex {
    ScreenRectVertex() = default;
    ScreenRectVertex(float x, float y, float u, float v)
        : position{Common::MakeVec(x, y)}, tex_coord{Common::MakeVec(u, v)} {}

    Common::Vec2f position;
    Common::Vec2f tex_coord;
};

constexpr u32 VERTEX_BUFFER_SIZE = sizeof(ScreenRectVertex) * 8192;

// The on-screen overlay gets its own ring, kept separate from the present vertex_buffer above.
// Sharing a ring causes memory exhaustion and thus the menu to overwrite its own tail,
// causing a crash.
constexpr u32 OVERLAY_VERTEX_BUFFER_SIZE = sizeof(float) * 4 * 32768;

constexpr std::array<f32, 4 * 4> MakeOrthographicMatrix(u32 width, u32 height) {
    // clang-format off
    return { 2.f / width, 0.f,         0.f, -1.f,
            0.f,         2.f / height, 0.f, -1.f,
            0.f,         0.f,          1.f,  0.f,
            0.f,         0.f,          0.f,  1.f};
    // clang-format on
}

constexpr static std::array<vk::DescriptorSetLayoutBinding, 1> PRESENT_BINDINGS = {{
    {0, vk::DescriptorType::eCombinedImageSampler, 3, vk::ShaderStageFlagBits::eFragment},
}};

namespace {
static bool IsLowRefreshRate() {
#if (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)
    if (!Settings::values.use_display_refresh_rate_detection) {
        LOG_INFO(Render_Vulkan, "Refresh rate detection is currently disabled via settings");
        return false;
    }
#ifdef __APPLE__
    // Apple's low power mode sometimes limits applications to 30fps without changing the refresh
    // rate, meaning the above code doesn't catch it.
    if (AppleUtils::IsLowPowerModeEnabled()) {
        LOG_WARNING(Render_Vulkan, "Apple's low power mode is enabled, assuming low application "
                                   "framerate. FIFO will be disabled");
        return true;
    }

    const auto cur_refresh_rate = AppleUtils::GetRefreshRate();
#elif defined(ENABLE_SDL2)
    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        LOG_ERROR(Render_Vulkan, "Attempted to check refresh rate via SDL, but failed because "
                                 "SDL_INIT_VIDEO wasn't initialized");
        return false;
    }

    SDL_DisplayMode cur_display_mode;
    SDL_GetCurrentDisplayMode(0, &cur_display_mode); // TODO: Multimonitor handling. -OS

    const auto cur_refresh_rate = cur_display_mode.refresh_rate;
#endif // ENABLE_SDL2

    if (cur_refresh_rate < SCREEN_REFRESH_RATE) {
        LOG_WARNING(Render_Vulkan,
                    "Detected refresh rate lower than the emulated 3DS screen: {}hz. FIFO will "
                    "be disabled",
                    cur_refresh_rate);
        return true;
    } else {
        LOG_INFO(Render_Vulkan, "Refresh rate is above emulated 3DS screen: {}hz. Good.",
                 cur_refresh_rate);
    }
#endif // (defined(__APPLE__) || defined(ENABLE_SDL2)) && !defined(HAVE_LIBRETRO)

    // We have no available method of checking refresh rate. Just assume that everything is fine :)
    return false;
}
} // Anonymous namespace

RendererVulkan::RendererVulkan(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : RendererBase{system, window, secondary_window}, memory{system.Memory()}, pica{pica_},
      instance{window, Settings::values.physical_device.GetValue()}, scheduler{instance},
      renderpass_cache{instance, scheduler},
      main_present_window{window, instance, scheduler, IsLowRefreshRate()},
      vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                    VERTEX_BUFFER_SIZE},
      overlay_vertex_buffer{instance, scheduler, vk::BufferUsageFlagBits::eVertexBuffer,
                            OVERLAY_VERTEX_BUFFER_SIZE},
      update_queue{instance}, rasterizer{memory,
                                         pica,
                                         system.CustomTexManager(),
                                         *this,
                                         render_window,
                                         instance,
                                         scheduler,
                                         renderpass_cache,
                                         update_queue,
                                         main_present_window.ImageCount()},
      present_heap{instance, scheduler.GetMasterSemaphore(), PRESENT_BINDINGS, 32} {
    CompileShaders();
    CreateOverlayFont();
    BuildLayouts();
    BuildPipelines();
    if (secondary_window) {
        secondary_present_window_ptr = std::make_unique<PresentWindow>(
            *secondary_window, instance, scheduler, IsLowRefreshRate());
    }
}

RendererVulkan::~RendererVulkan() {
    vk::Device device = instance.GetDevice();
    scheduler.Finish();
    main_present_window.WaitPresent();
    device.waitIdle();

    device.destroyShaderModule(present_vertex_shader);
    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        device.destroyPipeline(present_pipelines[i]);
        device.destroyShaderModule(present_shaders[i]);
    }

    for (auto& sampler : present_samplers) {
        device.destroySampler(sampler);
    }

    for (auto& info : screen_infos) {
        device.destroyImageView(info.texture.image_view);
        vmaDestroyImage(instance.GetAllocator(), info.texture.image, info.texture.allocation);
    }

    device.destroyPipeline(cursor_pipeline);
    device.destroyShaderModule(cursor_vertex_shader);
    device.destroyShaderModule(cursor_fragment_shader);

    device.destroyPipeline(overlay_pipeline);
    device.destroyShaderModule(overlay_vertex_shader);
    device.destroyShaderModule(overlay_fragment_shader);
    device.destroySampler(overlay_font_sampler);
    device.destroyImageView(overlay_font_view);
    vmaDestroyImage(instance.GetAllocator(), overlay_font_image, overlay_font_allocation);
}

std::array<bool, 3> RendererVulkan::GetPresentedScreens() const {
    // The screenshot pass runs off its own layout, so give it everything.
    if (settings.screenshot_requested.load(std::memory_order_relaxed)) {
        return {true, true, true};
    }

    std::array<bool, 3> presented{};
    const auto mark = [&presented](const Layout::FramebufferLayout& layout) {
        if (layout.top_screen_enabled) {
            if (layout.render_3d_mode == Settings::StereoRenderOption::Off) {
                const u32 eye = static_cast<u32>(Settings::values.mono_render_option.GetValue());
                presented[eye] = true;
            } else {
                presented[0] = presented[1] = true;
            }
        }
        presented[2] |= layout.bottom_screen_enabled;
    };

    mark(render_window.GetFramebufferLayout());
    if (secondary_window) {
        mark(secondary_window->GetFramebufferLayout());
    }
    return presented;
}

void RendererVulkan::PrepareRendertarget() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;
    const auto presented = GetPresentedScreens();
    for (u32 i = 0; i < 3; i++) {
        const u32 fb_id = i == 2 ? 1 : 0;
        const auto& framebuffer = framebuffer_config[fb_id];
        auto& texture = screen_infos[i].texture;

        // A hidden screen still has to hold a live view for the present descriptor set, so point it
        // back at its own texture; the cache surface it was showing may since have been retired.
        if (!presented[i] && texture.image_view) {
            screen_infos[i].image_view = texture.image_view;
            continue;
        }

        const auto color_fill = fb_id == 0 ? regs_lcd.color_fill_top : regs_lcd.color_fill_bottom;
        if (color_fill.is_enabled) {
            screen_infos[i].image_view = texture.image_view;
            FillScreen(color_fill.AsVector(), texture);
            continue;
        }

        if (texture.width != framebuffer.width || texture.height != framebuffer.height ||
            texture.format != framebuffer.color_format) {
            ConfigureFramebufferTexture(texture, framebuffer);
        }

        LoadFBToScreenInfo(framebuffer, screen_infos[i], i == 1);
    }
}

void RendererVulkan::PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout) {
    const auto sampler = present_samplers[!Settings::values.filter_mode.GetValue()];
    const auto present_set = present_heap.Commit();
    for (u32 index = 0; index < screen_infos.size(); index++) {
        update_queue.AddImageSampler(present_set, 0, index, screen_infos[index].image_view,
                                     sampler);
    }

    renderpass_cache.EndRendering();
    scheduler.Record([this, layout, frame, present_set,
                      renderpass = main_present_window.Renderpass(),
                      index = current_pipeline](vk::CommandBuffer cmdbuf) {
        const vk::Viewport viewport = {
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(layout.width),
            .height = static_cast<float>(layout.height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        const vk::Rect2D scissor = {
            .offset = {0, 0},
            .extent = {layout.width, layout.height},
        };

        cmdbuf.setViewport(0, viewport);
        cmdbuf.setScissor(0, scissor);

        const vk::ClearValue clear{.color = clear_color};
        const vk::PipelineLayout layout{*present_pipeline_layout};
        const vk::RenderPassBeginInfo renderpass_begin_info = {
            .renderPass = renderpass,
            .framebuffer = frame->framebuffer,
            .renderArea =
                vk::Rect2D{
                    .offset = {0, 0},
                    .extent = {frame->width, frame->height},
                },
            .clearValueCount = 1,
            .pClearValues = &clear,
        };

        cmdbuf.beginRenderPass(renderpass_begin_info, vk::SubpassContents::eInline);
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, present_pipelines[index]);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, layout, 0, present_set, {});
    });
}

void RendererVulkan::RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                                    bool flipped) {
    if (!Settings::values.use_skip_duplicate_frames.GetValue() ||
        Core::PerfStats::game_frames_updated) {
        Frame* frame = window.GetRenderFrame();

        if (layout.width != frame->width || layout.height != frame->height) {
            window.WaitPresent();
            scheduler.Finish();
            window.RecreateFrame(frame, layout.width, layout.height);
        }

        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
        clear_color.float32[3] = 1.0f;

        DrawScreens(frame, layout, flipped);
        scheduler.Flush(frame->render_ready);
        window.Present(frame);
        if ((secondaryWindowEnabled && isSecondaryWindow) || (!secondaryWindowEnabled)) {
            Core::PerfStats::game_frames_updated = false;
            screenRendered = true;
        }
    }
}

void RendererVulkan::LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer,
                                        ScreenInfo& screen_info, bool right_eye) {

    if (framebuffer.address_right1 == 0 || framebuffer.address_right2 == 0) {
        right_eye = false;
    }

    const PAddr framebuffer_addr =
        framebuffer.active_fb == 0
            ? (right_eye ? framebuffer.address_right1 : framebuffer.address_left1)
            : (right_eye ? framebuffer.address_right2 : framebuffer.address_left2);

    LOG_TRACE(Render_Vulkan, "0x{:08x} bytes from 0x{:08x}({}x{}), fmt {:x}",
              framebuffer.stride * framebuffer.height, framebuffer_addr, framebuffer.width.Value(),
              framebuffer.height.Value(), framebuffer.format);

    const u32 bpp = Pica::BytesPerPixel(framebuffer.color_format);
    const std::size_t pixel_stride = framebuffer.stride / bpp;

    ASSERT(pixel_stride * bpp == framebuffer.stride);
    ASSERT(pixel_stride % 4 == 0);

    if (!rasterizer.AccelerateDisplay(framebuffer, framebuffer_addr, static_cast<u32>(pixel_stride),
                                      screen_info)) {
        // Reset the screen info's display texture to its own permanent texture
        screen_info.image_view = screen_info.texture.image_view;
        screen_info.texcoords = {0.f, 0.f, 1.f, 1.f};

        ASSERT(false);
    }
}

void RendererVulkan::CompileShaders() {
    const vk::Device device = instance.GetDevice();
    const std::string_view preamble =
        instance.IsImageArrayDynamicIndexSupported() ? "#define ARRAY_DYNAMIC_INDEX" : "";
    present_vertex_shader =
        Compile(HostShaders::VULKAN_PRESENT_VERT, vk::ShaderStageFlagBits::eVertex, device);
    present_shaders[0] = Compile(HostShaders::VULKAN_PRESENT_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[1] = Compile(HostShaders::VULKAN_PRESENT_ANAGLYPH_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);
    present_shaders[2] = Compile(HostShaders::VULKAN_PRESENT_INTERLACED_FRAG,
                                 vk::ShaderStageFlagBits::eFragment, device, preamble);

    cursor_vertex_shader =
        Compile(HostShaders::VULKAN_CURSOR_VERT, vk::ShaderStageFlagBits::eVertex, device);
    cursor_fragment_shader =
        Compile(HostShaders::VULKAN_CURSOR_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    overlay_vertex_shader =
        Compile(HostShaders::VULKAN_OVERLAY_VERT, vk::ShaderStageFlagBits::eVertex, device);
    overlay_fragment_shader =
        Compile(HostShaders::VULKAN_OVERLAY_FRAG, vk::ShaderStageFlagBits::eFragment, device);

    auto properties = instance.GetPhysicalDevice().getProperties();
    for (std::size_t i = 0; i < present_samplers.size(); i++) {
        const vk::Filter filter_mode = i == 0 ? vk::Filter::eLinear : vk::Filter::eNearest;
        const vk::SamplerCreateInfo sampler_info = {
            .magFilter = filter_mode,
            .minFilter = filter_mode,
            .mipmapMode = vk::SamplerMipmapMode::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .anisotropyEnable = instance.IsAnisotropicFilteringSupported(),
            .maxAnisotropy = properties.limits.maxSamplerAnisotropy,
            .compareEnable = false,
            .compareOp = vk::CompareOp::eAlways,
            .borderColor = vk::BorderColor::eIntOpaqueBlack,
            .unnormalizedCoordinates = false,
        };

        present_samplers[i] = device.createSampler(sampler_info);
    }
}

void RendererVulkan::CreateOverlayFont() {
    vk::Device device = instance.GetDevice();

    // R8 coverage atlas holding the glyphs.
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8Unorm,
        .extent = {static_cast<u32>(OverlayFont::kAtlasWidth),
                   static_cast<u32>(OverlayFont::kAtlasHeight), 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
    };
    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
    };
    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);
    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &overlay_font_allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating overlay font atlas with error {}", result);
        UNREACHABLE();
    }
    overlay_font_image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = overlay_font_image,
        .viewType = vk::ImageViewType::e2D,
        .format = vk::Format::eR8Unorm,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    overlay_font_view = device.createImageView(view_info);

    const vk::SamplerCreateInfo sampler_info = {
        .magFilter = vk::Filter::eLinear,
        .minFilter = vk::Filter::eLinear,
        .mipmapMode = vk::SamplerMipmapMode::eNearest,
        .addressModeU = vk::SamplerAddressMode::eClampToEdge,
        .addressModeV = vk::SamplerAddressMode::eClampToEdge,
        .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        .anisotropyEnable = false,
        .compareEnable = false,
        .borderColor = vk::BorderColor::eFloatTransparentBlack,
        .unnormalizedCoordinates = false,
    };
    overlay_font_sampler = device.createSampler(sampler_info);

    const vk::DeviceSize atlas_size = sizeof(OverlayFont::kAtlas);
    const vk::BufferCreateInfo staging_info = {
        .size = atlas_size,
        .usage = vk::BufferUsageFlagBits::eTransferSrc,
    };
    const VmaAllocationCreateInfo staging_alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VkBuffer unsafe_staging{};
    VmaAllocation staging_allocation{};
    VmaAllocationInfo staging_mapped{};
    VkBufferCreateInfo unsafe_staging_info = static_cast<VkBufferCreateInfo>(staging_info);
    result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_staging_info, &staging_alloc_info,
                             &unsafe_staging, &staging_allocation, &staging_mapped);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating overlay font staging buffer with error {}",
                     result);
        UNREACHABLE();
    }
    std::memcpy(staging_mapped.pMappedData, OverlayFont::kAtlas, atlas_size);
    vk::Buffer staging_buffer{unsafe_staging};

    renderpass_cache.EndRendering();
    scheduler.Record([image = overlay_font_image, staging_buffer,
                      width = static_cast<u32>(OverlayFont::kAtlasWidth),
                      height = static_cast<u32>(OverlayFont::kAtlasHeight)](
                         vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const vk::ImageMemoryBarrier to_transfer = {
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_transfer);

        const vk::BufferImageCopy copy = {
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, 1},
        };
        cmdbuf.copyBufferToImage(staging_buffer, image, vk::ImageLayout::eTransferDstOptimal, copy);

        const vk::ImageMemoryBarrier to_shader = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader, {}, {}, {}, to_shader);
    });
    scheduler.Finish();
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, staging_allocation);

    // A persistent descriptor set bound to the atlas.
    const vk::DescriptorSetLayoutBinding binding = {
        .binding = 0,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
    };
    overlay_descriptor_layout = device.createDescriptorSetLayoutUnique({
        .bindingCount = 1,
        .pBindings = &binding,
    });
    const vk::DescriptorPoolSize pool_size = {
        .type = vk::DescriptorType::eCombinedImageSampler,
        .descriptorCount = 1,
    };
    overlay_descriptor_pool = device.createDescriptorPoolUnique({
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    });
    const vk::DescriptorSetLayout set_layout = *overlay_descriptor_layout;
    overlay_descriptor_set = device.allocateDescriptorSets({
        .descriptorPool = *overlay_descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &set_layout,
    })[0];

    const vk::DescriptorImageInfo image_desc = {
        .sampler = overlay_font_sampler,
        .imageView = overlay_font_view,
        .imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
    };
    const vk::WriteDescriptorSet write = {
        .dstSet = overlay_descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = vk::DescriptorType::eCombinedImageSampler,
        .pImageInfo = &image_desc,
    };
    device.updateDescriptorSets(write, {});
}

void RendererVulkan::BuildLayouts() {
    const vk::PushConstantRange push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(PresentUniformData),
    };

    const auto descriptor_set_layout = present_heap.Layout();
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    };
    present_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(layout_info);

    const vk::PipelineLayoutCreateInfo cursor_layout_info = {};
    cursor_pipeline_layout = instance.GetDevice().createPipelineLayoutUnique(cursor_layout_info);

    // The overlay samples the font atlas and takes a per-batch tint via push constant.
    const vk::PushConstantRange overlay_push_range = {
        .stageFlags = vk::ShaderStageFlagBits::eFragment,
        .offset = 0,
        .size = sizeof(float) * 4,
    };
    const vk::DescriptorSetLayout overlay_set_layout = *overlay_descriptor_layout;
    const vk::PipelineLayoutCreateInfo overlay_layout_info = {
        .setLayoutCount = 1,
        .pSetLayouts = &overlay_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &overlay_push_range,
    };
    overlay_pipeline_layout =
        instance.GetDevice().createPipelineLayoutUnique(overlay_layout_info);
}

void RendererVulkan::BuildPipelines() {
    const vk::VertexInputBindingDescription binding = {
        .binding = 0,
        .stride = sizeof(ScreenRectVertex),
        .inputRate = vk::VertexInputRate::eVertex,
    };

    const std::array attributes = {
        vk::VertexInputAttributeDescription{
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, position),
        },
        vk::VertexInputAttributeDescription{
            .location = 1,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = offsetof(ScreenRectVertex, tex_coord),
        },
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = static_cast<u32>(attributes.size()),
        .pVertexAttributeDescriptions = attributes.data(),
    };

    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = vk::PrimitiveTopology::eTriangleStrip,
        .primitiveRestartEnable = false,
    };

    const vk::PipelineRasterizationStateCreateInfo raster_state = {
        .depthClampEnable = false,
        .rasterizerDiscardEnable = false,
        .cullMode = vk::CullModeFlagBits::eNone,
        .frontFace = vk::FrontFace::eClockwise,
        .depthBiasEnable = false,
        .lineWidth = 1.0f,
    };

    const vk::PipelineMultisampleStateCreateInfo multisampling = {
        .rasterizationSamples = vk::SampleCountFlagBits::e1,
        .sampleShadingEnable = false,
    };

    const vk::PipelineColorBlendAttachmentState colorblend_attachment = {
        .blendEnable = true,
        .srcColorBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstColorBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .colorBlendOp = vk::BlendOp::eAdd,
        .srcAlphaBlendFactor = vk::BlendFactor::eConstantAlpha,
        .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusConstantAlpha,
        .alphaBlendOp = vk::BlendOp::eAdd,
        .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
    };

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable = false,
        .attachmentCount = 1,
        .pAttachments = &colorblend_attachment,
    };

    const vk::Viewport placeholder_viewport = vk::Viewport{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
    const vk::Rect2D placeholder_scissor = vk::Rect2D{{0, 0}, {1, 1}};
    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .viewportCount = 1,
        .pViewports = &placeholder_viewport,
        .scissorCount = 1,
        .pScissors = &placeholder_scissor,
    };

    const std::array dynamic_states = {
        vk::DynamicState::eBlendConstants,
        vk::DynamicState::eViewport,
        vk::DynamicState::eScissor,
    };

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    const vk::PipelineDepthStencilStateCreateInfo depth_info = {
        .depthTestEnable = false,
        .depthWriteEnable = false,
        .depthCompareOp = vk::CompareOp::eAlways,
        .depthBoundsTestEnable = false,
        .stencilTestEnable = false,
    };

    for (u32 i = 0; i < PRESENT_PIPELINES; i++) {
        const std::array shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = present_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = present_shaders[i],
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo pipeline_info = {
            .stageCount = static_cast<u32>(shader_stages.size()),
            .pStages = shader_stages.data(),
            .pVertexInputState = &vertex_input_info,
            .pInputAssemblyState = &input_assembly,
            .pViewportState = &viewport_info,
            .pRasterizationState = &raster_state,
            .pMultisampleState = &multisampling,
            .pDepthStencilState = &depth_info,
            .pColorBlendState = &color_blending,
            .pDynamicState = &dynamic_info,
            .layout = *present_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build present pipelines");
        present_pipelines[i] = pipeline;
    }

    // Build cursor pipeline (simple position-only, inverted color blending)
    {
        const vk::VertexInputBindingDescription cursor_binding = {
            .binding = 0,
            .stride = sizeof(float) * 2,
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const vk::VertexInputAttributeDescription cursor_attribute = {
            .location = 0,
            .binding = 0,
            .format = vk::Format::eR32G32Sfloat,
            .offset = 0,
        };

        const vk::PipelineVertexInputStateCreateInfo cursor_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &cursor_binding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &cursor_attribute,
        };

        const vk::PipelineInputAssemblyStateCreateInfo cursor_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo cursor_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo cursor_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState cursor_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eOneMinusDstColor,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcColor,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eZero,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo cursor_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &cursor_blend_attachment,
        };

        const vk::Viewport placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo cursor_viewport = {
            .viewportCount = 1,
            .pViewports = &placeholder_vp,
            .scissorCount = 1,
            .pScissors = &placeholder_sc,
        };

        const std::array cursor_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo cursor_dynamic = {
            .dynamicStateCount = static_cast<u32>(cursor_dynamic_states.size()),
            .pDynamicStates = cursor_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo cursor_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array cursor_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = cursor_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = cursor_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo cursor_pipeline_info = {
            .stageCount = static_cast<u32>(cursor_shader_stages.size()),
            .pStages = cursor_shader_stages.data(),
            .pVertexInputState = &cursor_vertex_input,
            .pInputAssemblyState = &cursor_input_assembly,
            .pViewportState = &cursor_viewport,
            .pRasterizationState = &cursor_raster,
            .pMultisampleState = &cursor_multisample,
            .pDepthStencilState = &cursor_depth,
            .pColorBlendState = &cursor_color_blending,
            .pDynamicState = &cursor_dynamic,
            .layout = *cursor_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, cursor_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build cursor pipeline");
        cursor_pipeline = pipeline;
    }

    // Build the FPS overlay pipeline.
    {
        const vk::VertexInputBindingDescription overlay_binding = {
            .binding = 0,
            .stride = sizeof(float) * 4,
            .inputRate = vk::VertexInputRate::eVertex,
        };

        const std::array overlay_attributes = {
            vk::VertexInputAttributeDescription{
                .location = 0,
                .binding = 0,
                .format = vk::Format::eR32G32Sfloat,
                .offset = 0,
            },
            vk::VertexInputAttributeDescription{
                .location = 1,
                .binding = 0,
                .format = vk::Format::eR32G32Sfloat,
                .offset = sizeof(float) * 2,
            },
        };

        const vk::PipelineVertexInputStateCreateInfo overlay_vertex_input = {
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &overlay_binding,
            .vertexAttributeDescriptionCount = static_cast<u32>(overlay_attributes.size()),
            .pVertexAttributeDescriptions = overlay_attributes.data(),
        };

        const vk::PipelineInputAssemblyStateCreateInfo overlay_input_assembly = {
            .topology = vk::PrimitiveTopology::eTriangleList,
            .primitiveRestartEnable = false,
        };

        const vk::PipelineRasterizationStateCreateInfo overlay_raster = {
            .depthClampEnable = false,
            .rasterizerDiscardEnable = false,
            .cullMode = vk::CullModeFlagBits::eNone,
            .frontFace = vk::FrontFace::eClockwise,
            .depthBiasEnable = false,
            .lineWidth = 1.0f,
        };

        const vk::PipelineMultisampleStateCreateInfo overlay_multisample = {
            .rasterizationSamples = vk::SampleCountFlagBits::e1,
            .sampleShadingEnable = false,
        };

        const vk::PipelineColorBlendAttachmentState overlay_blend_attachment = {
            .blendEnable = true,
            .srcColorBlendFactor = vk::BlendFactor::eSrcAlpha,
            .dstColorBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .colorBlendOp = vk::BlendOp::eAdd,
            .srcAlphaBlendFactor = vk::BlendFactor::eOne,
            .dstAlphaBlendFactor = vk::BlendFactor::eOneMinusSrcAlpha,
            .alphaBlendOp = vk::BlendOp::eAdd,
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA,
        };

        const vk::PipelineColorBlendStateCreateInfo overlay_color_blending = {
            .logicOpEnable = false,
            .attachmentCount = 1,
            .pAttachments = &overlay_blend_attachment,
        };

        const vk::Viewport overlay_placeholder_vp = {0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        const vk::Rect2D overlay_placeholder_sc = {{0, 0}, {1, 1}};
        const vk::PipelineViewportStateCreateInfo overlay_viewport = {
            .viewportCount = 1,
            .pViewports = &overlay_placeholder_vp,
            .scissorCount = 1,
            .pScissors = &overlay_placeholder_sc,
        };

        const std::array overlay_dynamic_states = {
            vk::DynamicState::eViewport,
            vk::DynamicState::eScissor,
        };

        const vk::PipelineDynamicStateCreateInfo overlay_dynamic = {
            .dynamicStateCount = static_cast<u32>(overlay_dynamic_states.size()),
            .pDynamicStates = overlay_dynamic_states.data(),
        };

        const vk::PipelineDepthStencilStateCreateInfo overlay_depth = {
            .depthTestEnable = false,
            .depthWriteEnable = false,
            .depthCompareOp = vk::CompareOp::eAlways,
            .depthBoundsTestEnable = false,
            .stencilTestEnable = false,
        };

        const std::array overlay_shader_stages = {
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eVertex,
                .module = overlay_vertex_shader,
                .pName = "main",
            },
            vk::PipelineShaderStageCreateInfo{
                .stage = vk::ShaderStageFlagBits::eFragment,
                .module = overlay_fragment_shader,
                .pName = "main",
            },
        };

        const vk::GraphicsPipelineCreateInfo overlay_pipeline_info = {
            .stageCount = static_cast<u32>(overlay_shader_stages.size()),
            .pStages = overlay_shader_stages.data(),
            .pVertexInputState = &overlay_vertex_input,
            .pInputAssemblyState = &overlay_input_assembly,
            .pViewportState = &overlay_viewport,
            .pRasterizationState = &overlay_raster,
            .pMultisampleState = &overlay_multisample,
            .pDepthStencilState = &overlay_depth,
            .pColorBlendState = &overlay_color_blending,
            .pDynamicState = &overlay_dynamic,
            .layout = *overlay_pipeline_layout,
            .renderPass = main_present_window.Renderpass(),
        };

        const auto [result, pipeline] =
            instance.GetDevice().createGraphicsPipeline({}, overlay_pipeline_info);
        ASSERT_MSG(result == vk::Result::eSuccess, "Unable to build overlay pipeline");
        overlay_pipeline = pipeline;
    }
}

void RendererVulkan::ConfigureFramebufferTexture(TextureInfo& texture,
                                                 const Pica::FramebufferConfig& framebuffer) {
    vk::Device device = instance.GetDevice();
    if (texture.image_view) {
        device.destroyImageView(texture.image_view);
    }
    if (texture.image) {
        vmaDestroyImage(instance.GetAllocator(), texture.image, texture.allocation);
    }

    const VideoCore::PixelFormat pixel_format =
        VideoCore::PixelFormatFromGPUPixelFormat(framebuffer.color_format);
    const vk::Format format = instance.GetTraits(pixel_format).native;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {framebuffer.width, framebuffer.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &texture.allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    texture.image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = texture.image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    texture.image_view = device.createImageView(view_info);

    texture.width = framebuffer.width;
    texture.height = framebuffer.height;
    texture.format = framebuffer.color_format;
}

void RendererVulkan::FillScreen(Common::Vec3<u8> color, const TextureInfo& texture) {
    // When loading some 3GX extensions, FillScreen may be called before texture image is available
    if (!texture.image) {
        return;
    }

    const vk::ClearColorValue clear_color = {
        .float32 =
            std::array{
                color.r() / 255.0f,
                color.g() / 255.0f,
                color.b() / 255.0f,
                1.0f,
            },
    };

    renderpass_cache.EndRendering();
    scheduler.Record([image = texture.image, clear_color](vk::CommandBuffer cmdbuf) {
        const vk::ImageSubresourceRange range = {
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = VK_REMAINING_MIP_LEVELS,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };

        const vk::ImageMemoryBarrier pre_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        const vk::ImageMemoryBarrier post_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eShaderRead | vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eFragmentShader,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barrier);

        cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal, clear_color, range);

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eFragmentShader,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);
    });
}

void RendererVulkan::ReloadPipeline(Settings::StereoRenderOption render_3d) {
    switch (render_3d) {
    case Settings::StereoRenderOption::Anaglyph:
        current_pipeline = 1;
        break;
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced:
        current_pipeline = 2;
        draw_info.reverse_interlaced = render_3d == Settings::StereoRenderOption::ReverseInterlaced;
        break;
    default:
        current_pipeline = 0;
        break;
    }
}

void RendererVulkan::DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                                      Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info = screen_infos[screen_id];
    const auto& texcoords = screen_info.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info.texture.width * scale_factor),
                        static_cast<f32>(screen_info.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y,
                                            float w, float h,
                                            Layout::DisplayOrientation orientation) {
    const ScreenInfo& screen_info_l = screen_infos[screen_id_l];
    const auto& texcoords = screen_info_l.texcoords;

    std::array<ScreenRectVertex, 4> vertices;
    switch (orientation) {
    case Layout::DisplayOrientation::Landscape:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.right),
        }};
        break;
    case Layout::DisplayOrientation::Portrait:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x + w, y + h, texcoords.top, texcoords.left),
        }};
        std::swap(h, w);
        break;
    case Layout::DisplayOrientation::LandscapeFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.bottom, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.left),
        }};
        break;
    case Layout::DisplayOrientation::PortraitFlipped:
        vertices = {{
            ScreenRectVertex(x, y, texcoords.top, texcoords.left),
            ScreenRectVertex(x + w, y, texcoords.bottom, texcoords.left),
            ScreenRectVertex(x, y + h, texcoords.top, texcoords.right),
            ScreenRectVertex(x + w, y + h, texcoords.bottom, texcoords.right),
        }};
        std::swap(h, w);
        break;
    default:
        LOG_ERROR(Render_Vulkan, "Unknown DisplayOrientation: {}", orientation);
        break;
    }

    const u64 size = sizeof(ScreenRectVertex) * vertices.size();
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    const u32 scale_factor = GetResolutionScaleFactor();
    draw_info.i_resolution =
        Common::MakeVec(static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        static_cast<f32>(screen_info_l.texture.height * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.width * scale_factor),
                        1.0f / static_cast<f32>(screen_info_l.texture.height * scale_factor));
    draw_info.o_resolution = Common::MakeVec(h, w, 1.0f / h, 1.0f / w);
    draw_info.screen_id_l = screen_id_l;
    draw_info.screen_id_r = screen_id_r;

    scheduler.Record([this, offset = offset, info = draw_info](vk::CommandBuffer cmdbuf) {
        const u32 first_vertex = static_cast<u32>(offset) / sizeof(ScreenRectVertex);
        cmdbuf.pushConstants(*present_pipeline_layout,
                             vk::ShaderStageFlagBits::eFragment | vk::ShaderStageFlagBits::eVertex,
                             0, sizeof(info), &info);

        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        cmdbuf.draw(4, 1, first_vertex, 0);
    });
}

void RendererVulkan::ApplySecondLayerOpacity(float alpha) {
    scheduler.Record([alpha](vk::CommandBuffer cmdbuf) {
        const std::array<float, 4> blend_constants = {0.0f, 0.0f, 0.0f, alpha};
        cmdbuf.setBlendConstants(blend_constants.data());
    });
}

void RendererVulkan::DrawTopScreen(const Layout::FramebufferLayout& layout,
                                   const Common::Rectangle<u32>& top_screen) {
    if (!layout.top_screen_enabled) {
        return;
    }
    int leftside, rightside;
    leftside = Settings::values.swap_eyes_3d.GetValue() ? 1 : 0;
    rightside = Settings::values.swap_eyes_3d.GetValue() ? 0 : 1;
    const float top_screen_left = static_cast<float>(top_screen.left);
    const float top_screen_top = static_cast<float>(top_screen.top);
    const float top_screen_width = static_cast<float>(top_screen.GetWidth());
    const float top_screen_height = static_cast<float>(top_screen.GetHeight());

    const auto orientation = layout.GetOrientation();
    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        const int eye = static_cast<int>(Settings::values.mono_render_option.GetValue());
        DrawSingleScreen(eye, top_screen_left, top_screen_top, top_screen_width, top_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySide: {
        DrawSingleScreen(leftside, top_screen_left / 2, top_screen_top, top_screen_width / 2,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, static_cast<float>((top_screen_left / 2) + (layout.width / 2)),
                         top_screen_top, top_screen_width / 2, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(rightside, top_screen_left + layout.width / 2, top_screen_top,
                         top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(leftside, top_screen_left, top_screen_top, top_screen_width,
                         top_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            rightside, top_screen_left - 2.0f * layout.cardboard.user_x_shift + (layout.width / 2),
            top_screen_top, top_screen_width, top_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(leftside, rightside, top_screen_left, top_screen_top,
                               top_screen_width, top_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawBottomScreen(const Layout::FramebufferLayout& layout,
                                      const Common::Rectangle<u32>& bottom_screen) {
    if (!layout.bottom_screen_enabled) {
        return;
    }

    const float bottom_screen_left = static_cast<float>(bottom_screen.left);
    const float bottom_screen_top = static_cast<float>(bottom_screen.top);
    const float bottom_screen_width = static_cast<float>(bottom_screen.GetWidth());
    const float bottom_screen_height = static_cast<float>(bottom_screen.GetHeight());

    const auto orientation = layout.GetOrientation();

    switch (layout.render_3d_mode) {
    case Settings::StereoRenderOption::Off: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);

        break;
    }
    case Settings::StereoRenderOption::SideBySide: // Bottom screen is identical on both sides
    {
        DrawSingleScreen(2, bottom_screen_left / 2, bottom_screen_top, bottom_screen_width / 2,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, static_cast<float>((bottom_screen_left / 2) + (layout.width / 2)),
                         bottom_screen_top, bottom_screen_width / 2, bottom_screen_height,
                         orientation);
        break;
    }
    case Settings::StereoRenderOption::SideBySideFull: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(2, bottom_screen_left + layout.width / 2, bottom_screen_top,
                         bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::CardboardVR: {
        DrawSingleScreen(2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                         bottom_screen_height, orientation);
        draw_info.layer = 1;
        DrawSingleScreen(
            2, bottom_screen_left - 2.0f * layout.cardboard.user_x_shift + (layout.width / 2),
            bottom_screen_top, bottom_screen_width, bottom_screen_height, orientation);
        break;
    }
    case Settings::StereoRenderOption::Anaglyph:
    case Settings::StereoRenderOption::Interlaced:
    case Settings::StereoRenderOption::ReverseInterlaced: {
        DrawSingleScreenStereo(2, 2, bottom_screen_left, bottom_screen_top, bottom_screen_width,
                               bottom_screen_height, orientation);
        break;
    }
    }
}

void RendererVulkan::DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout,
                                 bool flipped) {
    if (settings.bg_color_update_requested.exchange(false)) {
        clear_color.float32[0] = Settings::values.bg_red.GetValue();
        clear_color.float32[1] = Settings::values.bg_green.GetValue();
        clear_color.float32[2] = Settings::values.bg_blue.GetValue();
    }
    if (settings.shader_update_requested.exchange(false)) {
        ReloadPipeline(layout.render_3d_mode);
    }

    // Build overlay geometry before the present render pass opens otherwise a flush
    // can happen mid build causing a crash.
    renderpass_cache.EndRendering();
    OverlayDraw fps_overlay = PrepareFpsOverlay(layout);
    OverlayDraw shader_notice = PrepareShaderNotice(layout);
    OverlayDraw toast = PrepareToast(layout);
    OverlayDraw quick_menu = PrepareQuickMenu(layout);

    PrepareDraw(frame, layout);

    const auto& top_screen = layout.top_screen;
    const auto& bottom_screen = layout.bottom_screen;
    draw_info.modelview = MakeOrthographicMatrix(layout.width, layout.height);

    draw_info.layer = 0;

    // Apply the initial default opacity value; Needed to avoid flickering
    ApplySecondLayerOpacity(1.0f);

    if (!Settings::values.swap_screen.GetValue()) {
        DrawTopScreen(layout, top_screen);
        draw_info.layer = 0;
        if (layout.bottom_opacity < 1) {
            ApplySecondLayerOpacity(layout.bottom_opacity);
        }
        DrawBottomScreen(layout, bottom_screen);
    } else {
        DrawBottomScreen(layout, bottom_screen);
        draw_info.layer = 0;
        if (layout.top_opacity < 1) {
            ApplySecondLayerOpacity(layout.top_opacity);
        }
        DrawTopScreen(layout, top_screen);
    }

    if (layout.additional_screen_enabled) {
        const auto& additional_screen = layout.additional_screen;
        if (!layout.additional_screen_is_bottom) {
            DrawTopScreen(layout, additional_screen);
        } else {
            DrawBottomScreen(layout, additional_screen);
        }
    }

    DrawCursor(layout);

    RecordOverlay(std::move(fps_overlay));
    RecordOverlay(std::move(shader_notice));
    RecordOverlay(std::move(toast));
    RecordOverlay(std::move(quick_menu));

    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.endRenderPass(); });
}

void RendererVulkan::DrawCursor(const Layout::FramebufferLayout& layout) {
    const auto cursor = render_window.GetCursorInfo();
    if (!cursor.visible) {
        return;
    }

    const float buf_w = static_cast<float>(layout.width);
    const float buf_h = static_cast<float>(layout.height);

    const float abs_y = layout.bottom_screen.top + cursor.projected_y;
    const float cy = (abs_y / buf_h) * 2.0f - 1.0f;
    const float ratio = static_cast<float>(layout.bottom_screen.GetHeight()) / 30.0f;
    const float rw = ratio / buf_w;
    const float rh = ratio / buf_h;
    const float bt = (layout.bottom_screen.top / buf_h) * 2.0f - 1.0f;
    const float bb = (layout.bottom_screen.bottom / buf_h) * 2.0f - 1.0f;

    // One crosshair is 12 vertices. Labo VR gets a matching crosshair in the
    // right-eye copy so the pointer sits at a comfortable binocular depth.
    std::array<float, 48> vertices{};
    std::size_t vertex_data_size = 0;
    const auto append_crosshair = [&](float horizontal_offset) {
        const float abs_x = layout.bottom_screen.left + cursor.projected_x + horizontal_offset;
        const float cx = (abs_x / buf_w) * 2.0f - 1.0f;
        const float bl = ((layout.bottom_screen.left + horizontal_offset) / buf_w) * 2.0f - 1.0f;
        const float br = ((layout.bottom_screen.right + horizontal_offset) / buf_w) * 2.0f - 1.0f;

        const float vl = std::fmax(cx - rw / 5.0f, bl);
        const float vr = std::fmin(cx + rw / 5.0f, br);
        const float vt = std::fmax(cy - rh, bt);
        const float vb = std::fmin(cy + rh, bb);
        const float hl = std::fmax(cx - rw, bl);
        const float hr = std::fmin(cx + rw, br);
        const float ht = std::fmax(cy - rh / 5.0f, bt);
        const float hb = std::fmin(cy + rh / 5.0f, bb);

        const std::array<float, 24> crosshair{
            vl, vt, vr, vt, vr, vb, vl, vt, vr, vb, vl, vb,
            hl, ht, hr, ht, hr, hb, hl, ht, hr, hb, hl, hb,
        };
        std::copy(crosshair.begin(), crosshair.end(), vertices.begin() + vertex_data_size);
        vertex_data_size += crosshair.size();
    };

    append_crosshair(0.0f);
    if (layout.render_3d_mode == Settings::StereoRenderOption::CardboardVR) {
        append_crosshair(layout.width / 2.0f - 2.0f * layout.cardboard.user_x_shift);
    }

    const u64 size = vertex_data_size * sizeof(float);
    auto [data, offset, invalidate] = vertex_buffer.Map(size, 16);
    std::memcpy(data, vertices.data(), size);
    vertex_buffer.Commit(size);

    scheduler.Record([this, offset = offset, vertex_count = static_cast<u32>(vertex_data_size / 2),
                      pipeline = cursor_pipeline](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline);
        cmdbuf.bindVertexBuffers(0, vertex_buffer.Handle(), {0});
        const u32 first_vertex = static_cast<u32>(offset) / (sizeof(float) * 2);
        cmdbuf.draw(vertex_count, 1, first_vertex, 0);
    });
}

namespace {
// Builds overlay geometry in output-pixel coordinates, emitting position + atlas-UV vertices.
class OverlayBuilder {
public:
    OverlayBuilder(std::vector<float>& verts, float width, float height)
        : verts{verts}, inv_w{2.0f / width}, inv_h{2.0f / height} {}

    u32 VertexCount() const {
        return static_cast<u32>(verts.size() / kFloatsPerVertex);
    }

    void AddRect(float x0, float y0, float x1, float y1) {
        PushQuad(x0, y0, x1, y1, OverlayFont::kWhiteU, OverlayFont::kWhiteV, OverlayFont::kWhiteU,
                 OverlayFont::kWhiteV);
    }

    // Width in output pixels that a string occupies.
    static float Measure(std::string_view text, float scale) {
        float width = 0.0f;
        for (char c : text) {
            width += OverlayFont::GlyphFor(c).xadvance * scale;
        }
        return width;
    }

    // Draws a string with its line box's top-left at (ox, oy).
    void AddText(float ox, float oy, std::string_view text, float scale) {
        float pen = ox;
        for (char c : text) {
            const OverlayFont::Glyph& g = OverlayFont::GlyphFor(c);
            if (g.w > 0.0f && g.h > 0.0f) {
                const float qx = pen + g.xoff * scale;
                const float qy = oy + g.yoff * scale;
                PushQuad(qx, qy, qx + g.w * scale, qy + g.h * scale, g.u0, g.v0, g.u1, g.v1);
            }
            pen += g.xadvance * scale;
        }
    }

private:
    static constexpr int kFloatsPerVertex = 4;

    void PushQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
        const float l = x0 * inv_w - 1.0f;
        const float r = x1 * inv_w - 1.0f;
        const float t = y0 * inv_h - 1.0f;
        const float b = y1 * inv_h - 1.0f;
        verts.insert(verts.end(), {
                                      l, t, u0, v0, r, t, u1, v0, r, b, u1, v1,
                                      l, t, u0, v0, r, b, u1, v1, l, b, u0, v1,
                                  });
    }

    std::vector<float>& verts;
    float inv_w;
    float inv_h;
};
} // namespace

RendererVulkan::OverlayDraw RendererVulkan::PrepareFpsOverlay(
    const Layout::FramebufferLayout& layout) {
    if (!Settings::values.show_fps.GetValue()) {
        return {};
    }

    // Refresh the frame rate a couple of times a second so the reading stays legible.
    const auto now = std::chrono::steady_clock::now();
    if (overlay_last_update.time_since_epoch().count() == 0 ||
        now - overlay_last_update >= std::chrono::milliseconds(500)) {
#ifdef __SWITCH__
        overlay_game_fps = static_cast<float>(system.GetLastPerfStats().game_fps);
#else
        overlay_game_fps = static_cast<float>(system.GetAndResetPerfStats().game_fps);
#endif
        overlay_last_update = now;
    }

    char text[16];
    const int fps = std::clamp(static_cast<int>(std::lround(overlay_game_fps)), 0, 999);
    std::snprintf(text, sizeof(text), "FPS %d", fps);

    const float w = static_cast<float>(layout.width);
    const float h = static_cast<float>(layout.height);
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    // Font em size scaled to the output so the counter stays a consistent size everywhere.
    const float em = std::max(14.0f, std::round(h / 32.0f));
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float margin = std::round(em * 0.6f);
    const float pad = std::round(em * 0.35f);

    // A *tasteful* box around the menu.
    float ink_top = OverlayFont::kAscent;
    float ink_bottom = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        const OverlayFont::Glyph& g = OverlayFont::GlyphFor(*p);
        if (g.h > 0.0f) {
            ink_top = std::min(ink_top, g.yoff);
            ink_bottom = std::max(ink_bottom, g.yoff + g.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(256);
    OverlayBuilder builder{verts, w, h};

    const float text_w = OverlayBuilder::Measure(text, scale);

    builder.AddRect(margin - pad, margin + ink_top * scale - pad, margin + text_w + pad,
                    margin + ink_bottom * scale + pad);
    const u32 box_vertices = builder.VertexCount();

    builder.AddText(margin, margin, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - box_vertices;

    const u64 size = verts.size() * sizeof(float);
    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);

    constexpr std::array<float, 4> box_color = {0.0f, 0.0f, 0.0f, 0.55f};
    constexpr std::array<float, 4> text_color = {0.53f, 1.0f, 0.53f, 1.0f};

    OverlayDraw overlay;
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    overlay.batches.push_back({box_color, 0, box_vertices});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, box_vertices, glyph_vertices});
    }
    return overlay;
}

RendererVulkan::OverlayDraw RendererVulkan::PrepareShaderNotice(
    const Layout::FramebufferLayout& layout) {
    if (!Settings::values.show_shader_compile_notice.GetValue()) {
        return {};
    }
    const u32 pending = VideoCore::GetPendingShaderCompiles();
    const auto now = std::chrono::steady_clock::now();
    if (pending > 0) {
        shader_notice_until = now + std::chrono::milliseconds(500);
    }
    if (now >= shader_notice_until) {
        return {};
    }

    char text[32];
    if (pending > 0) {
        std::snprintf(text, sizeof(text), "Compiling shaders  %u", pending);
    } else {
        std::snprintf(text, sizeof(text), "Compiling shaders");
    }

    const float w = static_cast<float>(layout.width);
    const float h = static_cast<float>(layout.height);
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    const float em = std::max(14.0f, std::round(h / 32.0f));
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float margin = std::round(em * 0.6f);
    const float pad = std::round(em * 0.35f);

    float ink_top = OverlayFont::kAscent;
    float ink_bottom = 0.0f;
    for (const char* p = text; *p != '\0'; ++p) {
        const OverlayFont::Glyph& g = OverlayFont::GlyphFor(*p);
        if (g.h > 0.0f) {
            ink_top = std::min(ink_top, g.yoff);
            ink_bottom = std::max(ink_bottom, g.yoff + g.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(256);
    OverlayBuilder builder{verts, w, h};

    const float text_w = OverlayBuilder::Measure(text, scale);

    // Anchor the box to the bottom-left so it stays clear of the FPS counter.
    const float ox = margin;
    const float oy = h - margin - pad - ink_bottom * scale;

    builder.AddRect(ox - pad, oy + ink_top * scale - pad, ox + text_w + pad,
                    oy + ink_bottom * scale + pad);
    const u32 box_vertices = builder.VertexCount();

    builder.AddText(ox, oy, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - box_vertices;

    const u64 size = verts.size() * sizeof(float);
    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);

    constexpr std::array<float, 4> box_color = {0.0f, 0.0f, 0.0f, 0.55f};
    constexpr std::array<float, 4> text_color = {1.0f, 0.82f, 0.35f, 1.0f};

    OverlayDraw overlay;
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    overlay.batches.push_back({box_color, 0, box_vertices});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, box_vertices, glyph_vertices});
    }
    return overlay;
}

RendererVulkan::OverlayDraw RendererVulkan::PrepareToast(const Layout::FramebufferLayout& layout) {
    const std::string text = VideoCore::GetOverlayToast();
    if (text.empty()) {
        return {};
    }

    const float w = static_cast<float>(layout.width);
    const float h = static_cast<float>(layout.height);
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    const float em = std::max(16.0f, std::round(h / 28.0f));
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float margin = std::round(em * 1.2f);
    const float pad = std::round(em * 0.45f);

    float ink_top = OverlayFont::kAscent;
    float ink_bottom = 0.0f;
    for (const char c : text) {
        const OverlayFont::Glyph& g = OverlayFont::GlyphFor(c);
        if (g.h > 0.0f) {
            ink_top = std::min(ink_top, g.yoff);
            ink_bottom = std::max(ink_bottom, g.yoff + g.h);
        }
    }

    std::vector<float> verts;
    verts.reserve(512);
    OverlayBuilder builder{verts, w, h};

    const float text_w = OverlayBuilder::Measure(text, scale);

    // Bottom centre, above the shader notice's row.
    const float ox = std::round((w - text_w) / 2.0f);
    const float oy = h - margin - pad - ink_bottom * scale;

    builder.AddRect(ox - pad, oy + ink_top * scale - pad, ox + text_w + pad,
                    oy + ink_bottom * scale + pad);
    const u32 box_vertices = builder.VertexCount();

    builder.AddText(ox, oy, text, scale);
    const u32 glyph_vertices = builder.VertexCount() - box_vertices;

    const u64 size = verts.size() * sizeof(float);
    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);

    constexpr std::array<float, 4> box_color = {0.05f, 0.06f, 0.08f, 0.85f};
    constexpr std::array<float, 4> text_color = {1.0f, 1.0f, 1.0f, 1.0f};

    OverlayDraw overlay;
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    overlay.batches.push_back({box_color, 0, box_vertices});
    if (glyph_vertices > 0) {
        overlay.batches.push_back({text_color, box_vertices, glyph_vertices});
    }
    return overlay;
}

RendererVulkan::OverlayDraw RendererVulkan::PrepareQuickMenu(
    const Layout::FramebufferLayout& layout) {
    if (!VideoCore::IsOverlayMenuVisible()) {
        return {};
    }
    const VideoCore::OverlayMenuState state = VideoCore::GetOverlayMenuState();
    if (!state.visible) {
        return {};
    }

    const float w = static_cast<float>(layout.width);
    const float h = static_cast<float>(layout.height);
    if (w <= 0.0f || h <= 0.0f) {
        return {};
    }

    std::vector<float> verts;
    verts.reserve(2048);
    OverlayBuilder builder{verts, w, h};

    // Font em size scaled to the output so the menu is a consistent size everywhere.
    const float em = std::max(18.0f, std::round(h / 26.0f));
    const float title_em = std::round(em * 1.18f);
    const float scale = em / OverlayFont::kBakePixelHeight;
    const float title_scale = title_em / OverlayFont::kBakePixelHeight;
    const float line_h = OverlayFont::kLineHeight * scale;
    const float title_line_h = OverlayFont::kLineHeight * title_scale;
    const float row_h = std::round(line_h * 1.5f);
    const float footer_h = line_h;
    const float pad = std::round(em * 0.9f);
    const float sep_gap = std::round(row_h * 0.4f);
    const float col_gap = em * 1.4f;

    // Size the panel to its contents and centre it.
    float max_row_w = 0.0f;
    for (const auto& item : state.items) {
        float rw = OverlayBuilder::Measure(item.label, scale);
        if (!item.value.empty()) {
            rw += col_gap + OverlayBuilder::Measure(item.value, scale);
        }
        max_row_w = std::max(max_row_w, rw);
    }
    const int n = static_cast<int>(state.items.size());
    const float inner_w = std::max({max_row_w, OverlayBuilder::Measure(state.title, title_scale),
                                    OverlayBuilder::Measure(state.hint, scale)});
    const float panel_w = std::clamp(inner_w + 2.0f * pad, 0.35f * w, 0.92f * w);
    const float panel_h =
        pad + title_line_h + sep_gap + static_cast<float>(n) * row_h + sep_gap + footer_h + pad;
    const float panel_x0 = std::round((w - panel_w) / 2.0f);
    const float panel_y0 = std::round((h - panel_h) / 2.0f);
    const float panel_x1 = panel_x0 + panel_w;
    const float panel_y1 = panel_y0 + panel_h;

    std::vector<OverlayDraw::Batch> batches;
    const auto emit = [&](const std::array<float, 4>& color, u32 start) {
        const u32 count = builder.VertexCount() - start;
        if (count > 0) {
            batches.push_back({color, start, count});
        }
    };

    constexpr std::array<float, 4> c_dim = {0.0f, 0.0f, 0.0f, 0.55f};
    constexpr std::array<float, 4> c_panel = {0.10f, 0.11f, 0.14f, 0.96f};
    constexpr std::array<float, 4> c_accent = {0.30f, 0.34f, 0.45f, 0.9f};
    constexpr std::array<float, 4> c_highlight = {0.20f, 0.45f, 0.85f, 0.9f};
    constexpr std::array<float, 4> c_title = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr std::array<float, 4> c_row = {0.82f, 0.85f, 0.92f, 1.0f};
    constexpr std::array<float, 4> c_sel = {1.0f, 1.0f, 1.0f, 1.0f};
    constexpr std::array<float, 4> c_footer = {0.60f, 0.63f, 0.72f, 1.0f};

    // Dim the running game.
    {
        const u32 s = builder.VertexCount();
        builder.AddRect(0.0f, 0.0f, w, h);
        emit(c_dim, s);
    }
    {
        const u32 s = builder.VertexCount();
        builder.AddRect(panel_x0, panel_y0, panel_x1, panel_y1);
        emit(c_panel, s);
    }

    // Walk down the panel building each section's vertical extents.
    float pen_y = panel_y0 + pad;
    const float title_x =
        std::round(panel_x0 + (panel_w - OverlayBuilder::Measure(state.title, title_scale)) / 2.0f);
    const float title_y = pen_y;
    pen_y += title_line_h + sep_gap * 0.5f;
    const float underline_y = std::round(pen_y);
    pen_y += sep_gap * 0.5f;
    const float rows_top = pen_y;
    pen_y = rows_top + static_cast<float>(n) * row_h + sep_gap * 0.5f;
    const float footer_line_y = std::round(pen_y);
    pen_y += sep_gap * 0.5f;
    const float footer_y = pen_y;

    // Title underline and footer separator.
    {
        const u32 s = builder.VertexCount();
        const float lx0 = panel_x0 + pad;
        const float lx1 = panel_x1 - pad;
        const float th = std::max(1.0f, std::round(em / 12.0f));
        builder.AddRect(lx0, underline_y, lx1, underline_y + th);
        builder.AddRect(lx0, footer_line_y, lx1, footer_line_y + th);
        emit(c_accent, s);
    }

    const bool has_selection = n > 0 && state.selected >= 0 && state.selected < n;

    // Highlight bar behind the selected row.
    if (has_selection) {
        const u32 s = builder.VertexCount();
        const float top = rows_top + static_cast<float>(state.selected) * row_h;
        const float inset = std::round(row_h * 0.08f);
        builder.AddRect(panel_x0 + pad * 0.5f, top + inset, panel_x1 - pad * 0.5f,
                        top + row_h - inset);
        emit(c_highlight, s);
    }

    // Title.
    {
        const u32 s = builder.VertexCount();
        builder.AddText(title_x, title_y, state.title, title_scale);
        emit(c_title, s);
    }

    const auto add_row = [&](int i) {
        const auto& item = state.items[i];
        const float top = rows_top + static_cast<float>(i) * row_h;
        const float ty = std::round(top + (row_h - line_h) / 2.0f);
        builder.AddText(panel_x0 + pad, ty, item.label, scale);
        if (!item.value.empty()) {
            const float vx = panel_x1 - pad - OverlayBuilder::Measure(item.value, scale);
            builder.AddText(vx, ty, item.value, scale);
        }
    };

    // Draw non-selected rows, then the selected row brighter on top of its highlight.
    {
        const u32 s = builder.VertexCount();
        for (int i = 0; i < n; ++i) {
            if (i != state.selected) {
                add_row(i);
            }
        }
        emit(c_row, s);
    }
    if (has_selection) {
        const u32 s = builder.VertexCount();
        add_row(state.selected);
        emit(c_sel, s);
    }

    // Footer hint.
    if (!state.hint.empty()) {
        const u32 s = builder.VertexCount();
        const float fx =
            std::round(panel_x0 + (panel_w - OverlayBuilder::Measure(state.hint, scale)) / 2.0f);
        builder.AddText(fx, footer_y, state.hint, scale);
        emit(c_footer, s);
    }

    if (batches.empty()) {
        return {};
    }

    const u32 size = static_cast<u32>(verts.size() * sizeof(float));
    auto [data, offset, invalidate] = overlay_vertex_buffer.Map(size, 16);
    std::memcpy(data, verts.data(), size);
    overlay_vertex_buffer.Commit(size);

    OverlayDraw overlay;
    overlay.base_vertex = static_cast<u32>(offset) / (sizeof(float) * 4);
    overlay.batches = std::move(batches);
    return overlay;
}

void RendererVulkan::RecordOverlay(OverlayDraw overlay) {
    if (overlay.batches.empty()) {
        return;
    }
    scheduler.Record([this, base_vertex = overlay.base_vertex,
                      batches = std::move(overlay.batches)](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, overlay_pipeline);
        cmdbuf.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *overlay_pipeline_layout, 0,
                                  overlay_descriptor_set, {});
        cmdbuf.bindVertexBuffers(0, overlay_vertex_buffer.Handle(), {0});
        for (const auto& b : batches) {
            cmdbuf.pushConstants(*overlay_pipeline_layout, vk::ShaderStageFlagBits::eFragment, 0,
                                 static_cast<u32>(b.color.size() * sizeof(float)), b.color.data());
            cmdbuf.draw(b.count, 1, base_vertex + b.first, 0);
        }
    });
}

void RendererVulkan::SwapBuffers() {
    system.perf_stats->StartSwap();
    screenRendered = false;
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        secondaryWindowEnabled = true;
    } else {
        secondaryWindowEnabled = false;
    }
#endif

    const Layout::FramebufferLayout& layout = render_window.GetFramebufferLayout();
    PrepareRendertarget();
    RenderScreenshot();
    isSecondaryWindow = false;
    RenderToWindow(main_present_window, layout, false);
#ifndef ANDROID
    if (Settings::values.layout_option.GetValue() == Settings::LayoutOption::SeparateWindows) {
        ASSERT(secondary_window);
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif

#ifdef ANDROID
    if (secondary_window) {
        const auto& secondary_layout = secondary_window->GetFramebufferLayout();
        if (!secondary_present_window_ptr) {
            secondary_present_window_ptr = std::make_unique<PresentWindow>(
                *secondary_window, instance, scheduler, IsLowRefreshRate());
        }
        isSecondaryWindow = true;
        RenderToWindow(*secondary_present_window_ptr, secondary_layout, false);
        secondary_window->PollEvents();
    }
#endif
    if (!screenRendered) {
        scheduler.Finish();
    }

    system.perf_stats->EndSwap();
    rasterizer.TickFrame();
    EndFrame();
}

void RendererVulkan::RenderScreenshot() {
    if (!settings.screenshot_requested.exchange(false)) {
        return;
    }

    if (!TryRenderScreenshotWithHostMemory()) {
        RenderScreenshotWithStagingCopy();
    }

    settings.screenshot_complete_callback(false);
}

void RendererVulkan::RenderScreenshotWithStagingCopy() {
    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    const vk::BufferCreateInfo staging_buffer_info = {
        .size = width * height * 4,
        .usage = vk::BufferUsageFlagBits::eTransferDst,
    };

    const VmaAllocationCreateInfo alloc_create_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT |
                 VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkBuffer unsafe_buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo alloc_info;
    VkBufferCreateInfo unsafe_buffer_info = static_cast<VkBufferCreateInfo>(staging_buffer_info);

    VkResult result = vmaCreateBuffer(instance.GetAllocator(), &unsafe_buffer_info,
                                      &alloc_create_info, &unsafe_buffer, &allocation, &alloc_info);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }

    vk::Buffer staging_buffer{unsafe_buffer};

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record(
        [width, height, source_image = frame.image, staging_buffer](vk::CommandBuffer cmdbuf) {
            const vk::ImageMemoryBarrier read_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            const vk::ImageMemoryBarrier write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = VK_REMAINING_MIP_LEVELS,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            static constexpr vk::MemoryBarrier memory_write_barrier = {
                .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            };

            const vk::BufferImageCopy image_copy = {
                .bufferOffset = 0,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource =
                    {
                        .aspectMask = vk::ImageAspectFlagBits::eColor,
                        .mipLevel = 0,
                        .baseArrayLayer = 0,
                        .layerCount = 1,
                    },
                .imageOffset = {0, 0, 0},
                .imageExtent = {width, height, 1},
            };

            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                   vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
            cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                     staging_buffer, image_copy);
            cmdbuf.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
                vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
        });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // Copy backing image data to the QImage screenshot buffer
    std::memcpy(settings.screenshot_bits, alloc_info.pMappedData, staging_buffer_info.size);

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Destroy allocated resources
    vmaDestroyBuffer(instance.GetAllocator(), staging_buffer, allocation);
    vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);
}

bool RendererVulkan::TryRenderScreenshotWithHostMemory() {
    // If the host-memory import alignment matches the allocation granularity of the platform, then
    // the entire span of memory can be trivially imported
    const bool trivial_import =
        instance.IsExternalMemoryHostSupported() &&
        instance.GetMinImportedHostPointerAlignment() == Common::GetPageSize();
    if (!trivial_import) {
        return false;
    }

    const vk::Device device = instance.GetDevice();

    const Layout::FramebufferLayout layout{settings.screenshot_framebuffer_layout};
    const u32 width = layout.width;
    const u32 height = layout.height;

    // For a span of memory [x, x + s], import [AlignDown(x, alignment), AlignUp(x + s, alignment)]
    // and maintain an offset to the start of the data
    const u64 import_alignment = instance.GetMinImportedHostPointerAlignment();
    const uintptr_t address = reinterpret_cast<uintptr_t>(settings.screenshot_bits);
    void* aligned_pointer = reinterpret_cast<void*>(Common::AlignDown(address, import_alignment));
    const u64 offset = address % import_alignment;
    const u64 aligned_size = Common::AlignUp(offset + width * height * 4ull, import_alignment);

    // Buffer<->Image mapping for the imported imported buffer
    const vk::BufferImageCopy buffer_image_copy = {
        .bufferOffset = offset,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {width, height, 1},
    };

    const vk::MemoryHostPointerPropertiesEXT import_properties =
        device.getMemoryHostPointerPropertiesEXT(
            vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT, aligned_pointer);

    if (!import_properties.memoryTypeBits) {
        // Could not import memory
        return false;
    }

    const std::optional<u32> memory_type_index = FindMemoryType(
        instance.GetPhysicalDevice().getMemoryProperties(),
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent,
        import_properties.memoryTypeBits);

    if (!memory_type_index.has_value()) {
        // Could not find memory type index
        return false;
    }

    const vk::StructureChain<vk::MemoryAllocateInfo, vk::ImportMemoryHostPointerInfoEXT>
        allocation_chain = {
            vk::MemoryAllocateInfo{
                .allocationSize = aligned_size,
                .memoryTypeIndex = memory_type_index.value(),
            },
            vk::ImportMemoryHostPointerInfoEXT{
                .handleType = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
                .pHostPointer = aligned_pointer,
            },
        };

    // Import host memory
    const vk::UniqueDeviceMemory imported_memory =
        device.allocateMemoryUnique(allocation_chain.get());

    const vk::StructureChain<vk::BufferCreateInfo, vk::ExternalMemoryBufferCreateInfo> buffer_info =
        {
            vk::BufferCreateInfo{
                .size = aligned_size,
                .usage = vk::BufferUsageFlagBits::eTransferDst,
                .sharingMode = vk::SharingMode::eExclusive,
            },
            vk::ExternalMemoryBufferCreateInfo{
                .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eHostAllocationEXT,
            },
        };

    // Bind imported memory to buffer
    const vk::UniqueBuffer imported_buffer = device.createBufferUnique(buffer_info.get());
    device.bindBufferMemory(imported_buffer.get(), imported_memory.get(), 0);

    Frame frame{};
    main_present_window.RecreateFrame(&frame, width, height);

    DrawScreens(&frame, layout, false);

    scheduler.Record([buffer_image_copy, source_image = frame.image,
                      imported_buffer = imported_buffer.get()](vk::CommandBuffer cmdbuf) {
        const vk::ImageMemoryBarrier read_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const vk::ImageMemoryBarrier write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        static constexpr vk::MemoryBarrier memory_write_barrier = {
            .srcAccessMask = vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
        };

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, read_barrier);
        cmdbuf.copyImageToBuffer(source_image, vk::ImageLayout::eTransferSrcOptimal,
                                 imported_buffer, buffer_image_copy);
        cmdbuf.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
            vk::DependencyFlagBits::eByRegion, memory_write_barrier, {}, write_barrier);
    });

    // Ensure the copy is fully completed before saving the screenshot
    scheduler.Finish();

    // QImage::Format_RGB32 expects BGRA byte order. If the swapchain format is RGBA,
    // swap R and B channels so the screenshot colors are correct.
    if (main_present_window.GetSurfaceFormat() == vk::Format::eR8G8B8A8Unorm) {
        u8* pixels = static_cast<u8*>(settings.screenshot_bits);
        for (u32 i = 0; i < width * height; i++) {
            std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
        }
    }

    // Image data has been copied directly to host memory
    device.destroyFramebuffer(frame.framebuffer);
    device.destroyImageView(frame.image_view);

    return true;
}

void RendererVulkan::NotifySurfaceChanged(bool is_second_window) {
    if (is_second_window) {
        if (secondary_present_window_ptr) {
            secondary_present_window_ptr->NotifySurfaceChanged();
        }
    } else {
        main_present_window.NotifySurfaceChanged();
    }
}

} // namespace Vulkan
