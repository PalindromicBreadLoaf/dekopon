// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <exception>

#include "common/horizon_thread.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#include "vk_platform.h"

#include <vk_mem_alloc.h>

MICROPROFILE_DEFINE(Vulkan_WaitPresent, "Vulkan", "Wait For Present", MP_RGB(128, 128, 128));

namespace Vulkan {

namespace {

bool CanBlitToSwapchain(const vk::PhysicalDevice& physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                          s32 swapchain_height) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = swapchain_width,
                    .y = swapchain_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageCopy MakeImageCopy(u32 frame_width, u32 frame_height, u32 swapchain_width,
                                          u32 swapchain_height) {
    return vk::ImageCopy{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffset =
            vk::Offset3D{
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffset =
            vk::Offset3D{
                .x = 0,
                .y = 0,
                .z = 0,
            },
        .extent =
            vk::Extent3D{
                .width = std::min(frame_width, swapchain_width),
                .height = std::min(frame_height, swapchain_height),
                .depth = 1,
            },
    };
}

} // Anonymous namespace

PresentWindow::PresentWindow(Frontend::EmuWindow& emu_window_, const Instance& instance_,
                             Scheduler& scheduler_, bool low_refresh_rate_)
    : emu_window{emu_window_}, instance{instance_}, scheduler{scheduler_},
      low_refresh_rate{low_refresh_rate_},
      surface{CreateSurface(instance.GetInstance(), emu_window)}, next_surface{surface},
      swapchain{instance, emu_window.GetFramebufferLayout().width,
                emu_window.GetFramebufferLayout().height, surface, low_refresh_rate_},
      graphics_queue{instance.GetGraphicsQueue()}, present_renderpass{CreateRenderpass()},
      vsync_enabled{Settings::values.use_vsync.GetValue()},
      blit_supported{
          CanBlitToSwapchain(instance.GetPhysicalDevice(), swapchain.GetSurfaceFormat().format)},
      async_presentation{Settings::values.async_presentation.GetValue()},
      use_present_thread{async_presentation},
      last_render_surface{emu_window.GetWindowInfo().render_surface} {

    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    const vk::CommandPoolCreateInfo pool_info = {
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer |
                 vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    command_pool = device.createCommandPool(pool_info);

#ifdef ENABLE_LSFG
    constexpr u32 buffers_per_frame = 1 + kMaxGeneratedFrames;
#else
    constexpr u32 buffers_per_frame = 1;
#endif

    const vk::CommandBufferAllocateInfo alloc_info = {
        .commandPool = command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = num_images * buffers_per_frame,
    };
    const std::vector command_buffers = device.allocateCommandBuffers(alloc_info);

    swap_chain.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = swap_chain[i];
        frame.cmdbuf = command_buffers[i * buffers_per_frame];
#ifdef ENABLE_LSFG
        for (u32 j = 0; j < kMaxGeneratedFrames; j++) {
            frame.generated_cmdbufs[j] = command_buffers[i * buffers_per_frame + 1 + j];
        }
#endif
        frame.render_ready = device.createSemaphore({});
        frame.present_done = device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled});
#ifdef ENABLE_LSFG
        CreateOverlayBuffer(frame);
#endif
        free_queue.push(&frame);
    }

    if (instance.HasDebuggingToolAttached()) {
        for (u32 i = 0; i < num_images; ++i) {
            SetObjectName(device, swap_chain[i].cmdbuf, "Swapchain Command Buffer {}", i);
            SetObjectName(device, swap_chain[i].render_ready,
                          "Swapchain Semaphore: render_ready {}", i);
            SetObjectName(device, swap_chain[i].present_done, "Swapchain Fence: present_done {}",
                          i);
        }
    }

#ifdef ENABLE_LSFG
    overlay_renderpass = CreateOverlayRenderpass();
    RecreateOverlayTargets();
#endif

    if (use_present_thread) {
        present_thread = std::jthread([this](std::stop_token token) { PresentThread(token); });
    }
}

PresentWindow::~PresentWindow() {
    // The present thread records into the command pool and every frame destroyed below, so it has
    // to be gone before any of them are.
    present_thread.request_stop();
    if (present_thread.joinable()) {
        present_thread.join();
    }

    // Drain rather than submit.
    scheduler.WaitWorker();
    const vk::Device device = instance.GetDevice();
    device.waitIdle();
    device.destroyCommandPool(command_pool);
    device.destroyRenderPass(present_renderpass);
#ifdef ENABLE_LSFG
    for (auto framebuffer : overlay_framebuffers) {
        device.destroyFramebuffer(framebuffer);
    }
    for (auto view : overlay_views) {
        device.destroyImageView(view);
    }
    device.destroyRenderPass(overlay_renderpass);
#endif
    for (auto& frame : swap_chain) {
        device.destroyImageView(frame.image_view);
        device.destroyFramebuffer(frame.framebuffer);
        device.destroySemaphore(frame.render_ready);
        device.destroyFence(frame.present_done);
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
#ifdef ENABLE_LSFG
        if (frame.overlay_buffer) {
            vmaDestroyBuffer(instance.GetAllocator(), frame.overlay_buffer,
                             frame.overlay_allocation);
        }
#endif
    }
}

void PresentWindow::RecreateFrame(Frame* frame, u32 width, u32 height) {
    vk::Device device = instance.GetDevice();
    if (frame->framebuffer) {
        device.destroyFramebuffer(frame->framebuffer);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc,
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
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}", result);
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
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
    frame->image_view = device.createImageView(view_info);

    const vk::FramebufferCreateInfo framebuffer_info = {
        .renderPass = present_renderpass,
        .attachmentCount = 1,
        .pAttachments = &frame->image_view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    frame->framebuffer = instance.GetDevice().createFramebuffer(framebuffer_info);

    frame->width = width;
    frame->height = height;
    frame->present_width = width;
    frame->present_height = height;
}

Frame* PresentWindow::GetRenderFrame() {
    MICROPROFILE_SCOPE(Vulkan_WaitPresent);

    // Wait for free presentation frames
    std::unique_lock lock{free_mutex};
    free_cv.wait(lock, [this] { return !free_queue.empty(); });

    // Take the frame from the queue
    Frame* frame = free_queue.front();
    free_queue.pop();

    vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }

        // eErrorInitializationFailed occurs on Mali GPU drivers due to them
        // using the ppoll() syscall which isn't correctly restarted after a signal,
        // we need to manually retry waiting in that case
        if (result == vk::Result::eErrorInitializationFailed) {
            continue;
        }
    }

    device.resetFences(frame->present_done);
    return frame;
}

#ifdef ENABLE_LSFG
void PresentWindow::CreateOverlayBuffer(Frame& frame) {
    const VkBufferCreateInfo buffer_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = kOverlayBufferSize,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    const VmaAllocationCreateInfo alloc_info{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    };

    VkBuffer buffer{};
    VmaAllocationInfo mapping{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_info, &alloc_info, &buffer,
                        &frame.overlay_allocation, &mapping) != VK_SUCCESS) {
        LOG_WARNING(Render_Vulkan, "Could not allocate an overlay vertex buffer");
        return;
    }
    frame.overlay_buffer = vk::Buffer{buffer};
    frame.overlay_data = static_cast<u8*>(mapping.pMappedData);
}

vk::RenderPass PresentWindow::CreateOverlayRenderpass() {
    const vk::AttachmentReference color_ref = {
        .attachment = 0,
        .layout = vk::ImageLayout::eGeneral,
    };

    const vk::SubpassDescription subpass = {
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &color_ref,
    };

    const vk::AttachmentDescription color_attachment = {
        .format = swapchain.GetSurfaceFormat().format,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eGeneral,
        .finalLayout = vk::ImageLayout::ePresentSrcKHR,
    };

    const vk::RenderPassCreateInfo renderpass_info = {
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
    };

    return instance.GetDevice().createRenderPass(renderpass_info);
}

void PresentWindow::RecreateOverlayTargets() {
    const vk::Device device = instance.GetDevice();
    for (auto framebuffer : overlay_framebuffers) {
        device.destroyFramebuffer(framebuffer);
    }
    for (auto view : overlay_views) {
        device.destroyImageView(view);
    }
    overlay_framebuffers.clear();
    overlay_views.clear();

    const vk::Extent2D extent = swapchain.GetExtent();
    const vk::Format format = swapchain.GetSurfaceFormat().format;
    for (const vk::Image image : swapchain.GetImages()) {
        const vk::ImageViewCreateInfo view_info = {
            .image = image,
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
        const vk::ImageView view = device.createImageView(view_info);
        overlay_views.push_back(view);

        const vk::FramebufferCreateInfo framebuffer_info = {
            .renderPass = overlay_renderpass,
            .attachmentCount = 1,
            .pAttachments = &view,
            .width = extent.width,
            .height = extent.height,
            .layers = 1,
        };
        overlay_framebuffers.push_back(device.createFramebuffer(framebuffer_info));
    }
}

void PresentWindow::RecordOverlayPass(vk::CommandBuffer cmdbuf, const Frame* frame) {
    const u32 index = swapchain.GetImageIndex();
    if (index >= overlay_framebuffers.size()) {
        return;
    }

    const vk::Extent2D extent = swapchain.GetExtent();
    cmdbuf.setViewport(0, vk::Viewport{
                              .x = 0.0f,
                              .y = 0.0f,
                              .width = static_cast<float>(extent.width),
                              .height = static_cast<float>(extent.height),
                              .minDepth = 0.0f,
                              .maxDepth = 1.0f,
                          });
    cmdbuf.setScissor(0, vk::Rect2D{.offset = {0, 0}, .extent = extent});

    cmdbuf.beginRenderPass(
        vk::RenderPassBeginInfo{
            .renderPass = overlay_renderpass,
            .framebuffer = overlay_framebuffers[index],
            .renderArea = vk::Rect2D{.offset = {0, 0}, .extent = extent},
        },
        vk::SubpassContents::eInline);
    overlay_recorder->RecordOverlays(cmdbuf, frame->overlay_buffer, frame->overlays);
    cmdbuf.endRenderPass();
}

VideoCore::FrameGenerationDecision PresentWindow::ClassifyFrameGeneration() {
    auto decision = VideoCore::UpdateFrameGenerationGate();
    if (decision.state == VideoCore::FrameGenerationState::Active &&
        lsfg_unavailable.load(std::memory_order_relaxed)) {
        decision = {VideoCore::FrameGenerationState::Unavailable, 0};
    }
    return decision;
}
#endif

void PresentWindow::Present(Frame* frame) {
    bool generating = false;
#ifdef ENABLE_LSFG
    generating = frame->frame_gen.state == VideoCore::FrameGenerationState::Active;
#endif

    const bool async = async_presentation && !generating;
    if (async != use_present_thread) {
        WaitPresent();
        use_present_thread = async;
    }

    if (!use_present_thread) {
        scheduler.WaitWorker();
        CopyToSwapchain(frame);

        std::scoped_lock lock{free_mutex};
        free_queue.push(frame);
        free_cv.notify_one();
        return;
    }

    scheduler.Record([this, frame](vk::CommandBuffer) {
        std::unique_lock lock{queue_mutex};
        present_queue.push(frame);
        frame_cv.notify_one();
    });
}

void PresentWindow::WaitPresent() {
    if (!present_thread.joinable()) {
        return;
    }

    // Wait for the present queue to be empty
    {
        std::unique_lock queue_lock{queue_mutex};
        frame_cv.wait(queue_lock, [this] { return present_queue.empty(); });
    }

    // The above condition will be satisfied when the last frame is taken from the queue.
    // To ensure that frame has been presented as well take hold of the swapchain
    // mutex.
    std::scoped_lock swapchain_lock{swapchain_mutex};
}

void PresentWindow::PresentThread(std::stop_token token) {
    Common::SetCurrentThreadName("VulkanPresent");
    // See VulkanWorker: park presentation alongside submission.
    Common::Horizon::PinGraphicsSupportThread();
    while (!token.stop_requested()) {
        std::unique_lock lock{queue_mutex};

        // Wait for presentation frames
        Common::CondvarWait(frame_cv, lock, token, [this] { return !present_queue.empty(); });
        if (token.stop_requested()) {
            return;
        }

        // Take the frame and notify anyone waiting
        Frame* frame = present_queue.front();
        present_queue.pop();
        frame_cv.notify_one();

        // By exchanging the lock ownership we take the swapchain lock
        // before the queue lock goes out of scope. This way the swapchain
        // lock in WaitPresent is guaranteed to occur after here.
        std::exchange(lock, std::unique_lock{swapchain_mutex});

        CopyToSwapchain(frame);

        // Free the frame for reuse
        std::scoped_lock fl{free_mutex};
        free_queue.push(frame);
        free_cv.notify_one();
    }
}

void PresentWindow::NotifySurfaceChanged() {
#ifdef ANDROID
    std::scoped_lock lock{recreate_surface_mutex};
    next_surface = CreateSurface(instance.GetInstance(), emu_window);
    recreate_surface_cv.notify_one();
#endif
}

#ifdef ENABLE_LSFG
void PresentWindow::ResetFrameGeneration() {
    if (!lsfg_bridge) {
        return;
    }
    std::scoped_lock submit_lock{scheduler.submit_mutex};
    lsfg_bridge.reset();
}

void PresentWindow::UpdateFrameGeneration(Frame* frame) {
    using VideoCore::FrameGenerationState;

    if (frame->frame_gen.state == FrameGenerationState::Off) {
        ResetFrameGeneration();
        lsfg_attempted = false;
        lsfg_unavailable.store(false, std::memory_order_relaxed);
        VideoCore::PublishFrameGenerationDecision(frame->frame_gen);
        return;
    }

    const LsfgConfig config{
        .width = frame->width,
        .height = frame->height,
        .multiplier = frame->frame_gen.multiplier,
        .flow_scale = Settings::values.frame_generation_flow_scale.GetValue(),
        .performance_mode = Settings::values.frame_generation_performance_mode.GetValue(),
    };

    if (frame->frame_gen.state == FrameGenerationState::Unavailable) {
        if (lsfg_config != config) {
            lsfg_attempted = false;
            lsfg_unavailable.store(false, std::memory_order_relaxed);
        }
        VideoCore::PublishFrameGenerationDecision(frame->frame_gen);
        return;
    }

    if (frame->frame_gen.state != FrameGenerationState::Active) {
        if (lsfg_bridge) {
            lsfg_bridge->ResetHistory();
        }
        VideoCore::PublishFrameGenerationDecision(frame->frame_gen);
        return;
    }

    if (lsfg_attempted && lsfg_config == config) {
        if (!lsfg_bridge) {
            frame->frame_gen = {FrameGenerationState::Unavailable, 0};
        }
        VideoCore::PublishFrameGenerationDecision(frame->frame_gen);
        return;
    }

    ResetFrameGeneration();
    lsfg_attempted = true;
    lsfg_config = config;

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    if (format != vk::Format::eR8G8B8A8Unorm) {
        LOG_WARNING(Render_Vulkan,
                    "Frame generation runs on R8G8B8A8_UNORM but the swapchain is {}",
                    vk::to_string(format));
    }

    const LsfgBridgeInfo info{
        .instance = instance.GetInstance(),
        .physical_device = instance.GetPhysicalDevice(),
        .device = instance.GetDevice(),
        .queue = graphics_queue,
        .queue_family_index = instance.GetGraphicsQueueFamilyIndex(),
        .width = config.width,
        .height = config.height,
        .generated_frames = config.multiplier - 1,
        .flow_scale = static_cast<float>(config.flow_scale) / 100.0f,
        .performance_mode = config.performance_mode,
    };

    {
        std::scoped_lock submit_lock{scheduler.submit_mutex};
        lsfg_bridge = CreateLsfgBridge(info);
    }

    lsfg_unavailable.store(lsfg_bridge == nullptr, std::memory_order_relaxed);
    if (!lsfg_bridge) {
        frame->frame_gen = {FrameGenerationState::Unavailable, 0};
    }
    VideoCore::PublishFrameGenerationDecision(frame->frame_gen);
}

bool PresentWindow::CopyToSwapchainGenerated(Frame* frame) {
    std::array<VkImage, kMaxGeneratedFrames> generated{};
    u32 generated_count = 0;
    try {
        std::scoped_lock submit_lock{scheduler.submit_mutex};
        generated_count = lsfg_bridge->RecordFrame(frame->image, frame->render_ready, generated);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Frame generation failed {}",
                  e.what());
        ResetFrameGeneration();
        lsfg_unavailable.store(true, std::memory_order_relaxed);
        VideoCore::PublishFrameGenerationDecision(
            {VideoCore::FrameGenerationState::Unavailable, 0});
        return false;
    }

    const bool overlays =
        frame->overlays_deferred && overlay_recorder != nullptr && frame->overlay_buffer;

    const auto blit_and_present = [&](vk::CommandBuffer cmdbuf, const BlitSource& source,
                                      vk::Fence fence) {
        AcquireSwapchainImage(frame->present_width, frame->present_height);
        cmdbuf.begin(vk::CommandBufferBeginInfo{
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
        });
        RecordBlitToSwapchain(cmdbuf, source, swapchain.Image(), overlays);
        if (overlays) {
            RecordOverlayPass(cmdbuf, frame);
        }
        cmdbuf.end();
        SubmitAndPresent(cmdbuf, VK_NULL_HANDLE, fence);
    };

    for (u32 i = 0; i < generated_count; i++) {
        blit_and_present(frame->generated_cmdbufs[i],
                         BlitSource{
                             .image = vk::Image{generated[i]},
                             .width = frame->width,
                             .height = frame->height,
                             .layout = vk::ImageLayout::eGeneral,
                             .access = vk::AccessFlagBits::eShaderWrite,
                             .stage = vk::PipelineStageFlagBits::eComputeShader,
                         },
                         VK_NULL_HANDLE);
    }

    blit_and_present(frame->cmdbuf,
                     BlitSource{
                         .image = frame->image,
                         .width = frame->width,
                         .height = frame->height,
                         .layout = vk::ImageLayout::eTransferSrcOptimal,
                         .access = vk::AccessFlagBits::eMemoryRead,
                         .stage = vk::PipelineStageFlagBits::eAllCommands,
                     },
                     frame->present_done);
    return true;
}
#endif

void PresentWindow::RecreateSwapchain(u32 width, u32 height) {
#ifdef ANDROID
    {
        std::unique_lock lock{recreate_surface_mutex};
        recreate_surface_cv.wait(lock, [this]() { return surface != next_surface; });
        surface = next_surface;
    }
#endif
    std::scoped_lock submit_lock{scheduler.submit_mutex};
    graphics_queue.waitIdle();
    swapchain.Create(width, height, surface, low_refresh_rate);
#ifdef ENABLE_LSFG
    RecreateOverlayTargets();
#endif
}

void PresentWindow::AcquireSwapchainImage(u32 width, u32 height) {
    while (!swapchain.AcquireNextImage()) {
        RecreateSwapchain(width, height);
    }
}

void PresentWindow::RecordBlitToSwapchain(vk::CommandBuffer cmdbuf, const BlitSource& source,
                                          vk::Image swapchain_image, bool overlay_follows) {
    const vk::ImageSubresourceRange subresource_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const std::array pre_barriers{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
            .oldLayout = vk::ImageLayout::eUndefined,
            .newLayout = vk::ImageLayout::eTransferDstOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = subresource_range,
        },
        vk::ImageMemoryBarrier{
            .srcAccessMask = source.access,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = source.layout,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source.image,
            .subresourceRange = subresource_range,
        },
    };

    std::array post_barriers{
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = overlay_follows ? vk::AccessFlagBits::eColorAttachmentWrite |
                                                   vk::AccessFlagBits::eColorAttachmentRead
                                             : vk::AccessFlagBits::eMemoryRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout =
                overlay_follows ? vk::ImageLayout::eGeneral : vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange = subresource_range,
        },
        vk::ImageMemoryBarrier{
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = source.access,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = source.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source.image,
            .subresourceRange = subresource_range,
        },
    };
    const u32 post_barrier_count =
        source.layout == vk::ImageLayout::eTransferSrcOptimal ? 1u : 2u;

    cmdbuf.pipelineBarrier(source.stage, vk::PipelineStageFlagBits::eTransfer,
                           vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

    const vk::Extent2D extent = swapchain.GetExtent();
    if (blit_supported) {
        cmdbuf.blitImage(source.image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlit(source.width, source.height, extent.width, extent.height),
                         vk::Filter::eLinear);
    } else {
        cmdbuf.copyImage(source.image, vk::ImageLayout::eTransferSrcOptimal, swapchain_image,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageCopy(source.width, source.height, extent.width, extent.height));
    }

    cmdbuf.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eAllCommands,
        vk::DependencyFlagBits::eByRegion, {}, {},
        vk::ArrayProxy<const vk::ImageMemoryBarrier>{post_barrier_count, post_barriers.data()});
}

void PresentWindow::SubmitAndPresent(vk::CommandBuffer cmdbuf, vk::Semaphore render_ready,
                                     vk::Fence fence) {
    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eAllGraphics,
    };

    const vk::Semaphore present_ready = swapchain.GetPresentReadySemaphore();
    const std::array wait_semaphores = {swapchain.GetImageAcquiredSemaphore(), render_ready};

    const vk::SubmitInfo submit_info = {
        .waitSemaphoreCount = render_ready ? 2u : 1u,
        .pWaitSemaphores = wait_semaphores.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1u,
        .pCommandBuffers = &cmdbuf,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &present_ready,
    };

    std::scoped_lock submit_lock{scheduler.submit_mutex, recreate_surface_mutex};

    try {
        graphics_queue.submit(submit_info, fence);
    } catch (vk::DeviceLostError& err) {
        LOG_CRITICAL(Render_Vulkan, "Device lost during present submit: {}", err.what());
        UNREACHABLE();
    }

    swapchain.Present();
}

void PresentWindow::CopyToSwapchain(Frame* frame) {
#ifndef ANDROID
    const bool use_vsync = Settings::values.use_vsync.GetValue();
    const bool size_changed = swapchain.GetWidth() != frame->present_width ||
                              swapchain.GetHeight() != frame->present_height;
    const bool vsync_changed = vsync_enabled != use_vsync;
    if (vsync_changed || size_changed) [[unlikely]] {
        vsync_enabled = use_vsync;
        RecreateSwapchain(frame->present_width, frame->present_height);
    }
#endif

#ifdef ENABLE_LSFG
    UpdateFrameGeneration(frame);
    if (frame->frame_gen.state == VideoCore::FrameGenerationState::Active &&
        CopyToSwapchainGenerated(frame)) {
        return;
    }
#endif

    AcquireSwapchainImage(frame->present_width, frame->present_height);

    bool overlays = false;
#ifdef ENABLE_LSFG
    overlays = frame->overlays_deferred && overlay_recorder != nullptr && frame->overlay_buffer;
#endif

    const vk::CommandBuffer cmdbuf = frame->cmdbuf;
    cmdbuf.begin(vk::CommandBufferBeginInfo{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });
    RecordBlitToSwapchain(cmdbuf,
                          BlitSource{
                              .image = frame->image,
                              .width = frame->width,
                              .height = frame->height,
                              .layout = vk::ImageLayout::eTransferSrcOptimal,
                              .access = vk::AccessFlagBits::eColorAttachmentWrite,
                              .stage = vk::PipelineStageFlagBits::eColorAttachmentOutput,
                          },
                          swapchain.Image(), overlays);
#ifdef ENABLE_LSFG
    if (overlays) {
        RecordOverlayPass(cmdbuf, frame);
    }
#endif
    cmdbuf.end();

    SubmitAndPresent(cmdbuf, frame->render_ready, frame->present_done);
}

vk::RenderPass PresentWindow::CreateRenderpass() {
    const vk::AttachmentReference color_ref = {
        .attachment = 0,
        .layout = vk::ImageLayout::eGeneral,
    };

    const vk::SubpassDescription subpass = {
        .pipelineBindPoint = vk::PipelineBindPoint::eGraphics,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &color_ref,
        .pResolveAttachments = 0,
        .pDepthStencilAttachment = nullptr,
    };

    const vk::AttachmentDescription color_attachment = {
        .format = swapchain.GetSurfaceFormat().format,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .stencilLoadOp = vk::AttachmentLoadOp::eDontCare,
        .stencilStoreOp = vk::AttachmentStoreOp::eDontCare,
        .initialLayout = vk::ImageLayout::eUndefined,
        .finalLayout = vk::ImageLayout::eTransferSrcOptimal,
    };

    const vk::RenderPassCreateInfo renderpass_info = {
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };

    return instance.GetDevice().createRenderPass(renderpass_info);
}

} // namespace Vulkan
