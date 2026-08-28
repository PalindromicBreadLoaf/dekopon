// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <vector>
#include "common/color.h"
#include "common/logging/log.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/memory.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_deko3d/dk_shader_compiler.h"
#include "video_core/renderer_deko3d/renderer_deko3d.h"

namespace Deko3D {

namespace {
constexpr u32 CmdBufMemorySize = 256 * 1024;
// CPU-visible scratch the LCD framebuffers are decoded into before the copy.
constexpr u32 StagingMemorySize = 2 * 1024 * 1024;
constexpr u32 VertexMemorySize = 64 * 1024;
constexpr u32 StagingAlignment = 256;

// One image descriptor per screen plus a single shared sampler in slot 0.
constexpr u32 NumImageDescriptors = RendererDeko3D::NumScreens;
constexpr u32 NumSamplers = 1;
constexpr u32 SamplerSlot = 0;

// Background fill behind the screens.
// I need to remember to remove this.
constexpr float ClearColor[4] = {1.0f, 0.45f, 0.1f, 1.0f};

struct PresentVertex {
    float position[2];
    float tex_coord[2];
};

// Check for the uam runtime compiler.
void UamSelfTest() {
    static constexpr char kSource[] = R"(#version 460
        layout(location = 0) out vec4 out_color;
        layout(std140, binding = 0) uniform Block { vec4 tint; };
        void main() { out_color = tint; }
    )";
    const auto blob =
        ShaderCompiler::CompileShader(ShaderCompiler::Stage::Fragment, kSource, "uam_selftest");
    if (blob) {
        LOG_INFO(Render, "deko3d: uam runtime compiler online ({} byte DKSH)", blob->size());
    } else {
        LOG_ERROR(Render, "deko3d: uam runtime compiler self-test FAILED");
    }
}

/// Decodes one row of a 3DS framebuffer into RGBA8.
void DecodeRow(Pica::PixelFormat format, const u8* src, u8* dst, u32 width) {
    const u32 bpp = Pica::BytesPerPixel(format);
    for (u32 x = 0; x < width; ++x) {
        const u8* pixel = src + x * bpp;
        Common::Vec4<u8> color;
        switch (format) {
        case Pica::PixelFormat::RGBA8:
            color = Common::Color::DecodeRGBA8(pixel);
            break;
        case Pica::PixelFormat::RGB8:
            color = Common::Color::DecodeRGB8(pixel);
            break;
        case Pica::PixelFormat::RGB565:
            color = Common::Color::DecodeRGB565(pixel);
            break;
        case Pica::PixelFormat::RGB5A1:
            color = Common::Color::DecodeRGB5A1(pixel);
            break;
        case Pica::PixelFormat::RGBA4:
            color = Common::Color::DecodeRGBA4(pixel);
            break;
        default:
            color = {0, 0, 0, 255};
            break;
        }
        dst[x * 4 + 0] = color.r();
        dst[x * 4 + 1] = color.g();
        dst[x * 4 + 2] = color.b();
        dst[x * 4 + 3] = color.a();
    }
}
} // namespace

RendererDeko3D::RendererDeko3D(Core::System& system, Pica::PicaCore& pica_,
                               Frontend::EmuWindow& window, Frontend::EmuWindow* secondary_window)
    : VideoCore::RendererBase{system, window, secondary_window}, pica{pica_},
      rasterizer{system.Memory(), pica} {
    // Deko3D stores the opaque nwindow here.
    void* native_window = window.GetWindowInfo().render_surface;
    if (native_window == nullptr) {
        LOG_ERROR(Render, "Deko3d: no native window provided.");
        return;
    }
    InitDeko3D(native_window);
}

RendererDeko3D::~RendererDeko3D() {
    ExitDeko3D();
}

void RendererDeko3D::InitDeko3D(void* native_window) {
    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    device = dkDeviceCreate(&device_maker);

    // Lay out the swapchain framebuffers.
    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, device);
    layout_maker.flags =
        DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layout_maker.format = DkImageFormat_RGBA8_Unorm;
    layout_maker.dimensions[0] = fb_width;
    layout_maker.dimensions[1] = fb_height;

    DkImageLayout framebuffer_layout;
    dkImageLayoutInitialize(&framebuffer_layout, &layout_maker);

    const u32 fb_size = AlignUp(static_cast<u32>(dkImageLayoutGetSize(&framebuffer_layout)),
                                dkImageLayoutGetAlignment(&framebuffer_layout));

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, device, NumFramebuffers * fb_size);
    memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    fb_memblock = dkMemBlockCreate(&memblock_maker);

    std::array<const DkImage*, NumFramebuffers> swapchain_images{};
    for (unsigned i = 0; i < NumFramebuffers; ++i) {
        dkImageInitialize(&framebuffers[i], &framebuffer_layout, fb_memblock, i * fb_size);
        swapchain_images[i] = &framebuffers[i];
    }

    DkSwapchainMaker swapchain_maker;
    dkSwapchainMakerDefaults(&swapchain_maker, device, native_window, swapchain_images.data(),
                             NumFramebuffers);
    swapchain = dkSwapchainCreate(&swapchain_maker);

    // Command buffer.
    dkMemBlockMakerDefaults(&memblock_maker, device, CmdBufMemorySize);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    cmdbuf_memblock = dkMemBlockCreate(&memblock_maker);

    DkCmdBufMaker cmdbuf_maker;
    dkCmdBufMakerDefaults(&cmdbuf_maker, device);
    cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
    dkCmdBufAddMemory(cmdbuf, cmdbuf_memblock, 0, CmdBufMemorySize);

    // Graphics queue.
    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, device);
    queue_maker.flags = DkQueueFlags_Graphics;
    queue = dkQueueCreate(&queue_maker);

    // Image descriptors followed by the sampler descriptors.
    const u32 descriptor_size = AlignUp(
        NumImageDescriptors * static_cast<u32>(sizeof(DkImageDescriptor)) +
            NumSamplers * static_cast<u32>(sizeof(DkSamplerDescriptor)),
        DK_MEMBLOCK_ALIGNMENT);
    dkMemBlockMakerDefaults(&memblock_maker, device, descriptor_size);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    descriptor_memblock = dkMemBlockCreate(&memblock_maker);
    const DkGpuAddr descriptor_base = dkMemBlockGetGpuAddr(descriptor_memblock);
    image_descriptor_set = descriptor_base;
    sampler_descriptor_set =
        descriptor_base + NumImageDescriptors * static_cast<u32>(sizeof(DkImageDescriptor));

    // Upload staging and vertex streaming buffers.
    dkMemBlockMakerDefaults(&memblock_maker, device, StagingMemorySize);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    staging_memblock = dkMemBlockCreate(&memblock_maker);

    dkMemBlockMakerDefaults(&memblock_maker, device, VertexMemorySize);
    memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    vertex_memblock = dkMemBlockCreate(&memblock_maker);

    shaders_ok = LoadShaders();
    if (!shaders_ok) {
        LOG_ERROR(Render, "deko3d: present shaders unavailable. Things will be broken.");
    }
    SetupSampler();

    initialized = true;
    LOG_INFO(Render, "deko3d device initialized ({}x{}, {} framebuffers, shaders={})", fb_width,
             fb_height, NumFramebuffers, shaders_ok);

    UamSelfTest();
}

bool RendererDeko3D::LoadShaders() {
    struct ShaderSpec {
        DkShader& shader;
        const char* path;
    };
    const std::array<ShaderSpec, 2> specs{{
        {present_vsh, "romfs:/shaders/present_vsh.dksh"},
        {present_fsh, "romfs:/shaders/present_fsh.dksh"},
    }};

    struct LoadedBlob {
        std::vector<u8> data;
        u32 offset;
    };
    std::array<LoadedBlob, specs.size()> blobs{};
    u32 total_size = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        std::FILE* file = std::fopen(specs[i].path, "rb");
        if (file == nullptr) {
            LOG_ERROR(Render, "deko3d: could not open shader {}", specs[i].path);
            return false;
        }
        std::fseek(file, 0, SEEK_END);
        const long size = std::ftell(file);
        std::rewind(file);
        if (size <= 0) {
            std::fclose(file);
            return false;
        }
        blobs[i].data.resize(static_cast<std::size_t>(size));
        const std::size_t read =
            std::fread(blobs[i].data.data(), 1, static_cast<std::size_t>(size), file);
        std::fclose(file);
        if (read != static_cast<std::size_t>(size)) {
            return false;
        }
        blobs[i].offset = total_size;
        total_size += AlignUp(static_cast<u32>(size), DK_SHADER_CODE_ALIGNMENT);
    }

    const u32 code_size = AlignUp(total_size, DK_MEMBLOCK_ALIGNMENT);
    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, device, code_size);
    memblock_maker.flags =
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    code_memblock = dkMemBlockCreate(&memblock_maker);
    if (code_memblock == nullptr) {
        return false;
    }

    u8* const code_base = static_cast<u8*>(dkMemBlockGetCpuAddr(code_memblock));
    for (std::size_t i = 0; i < specs.size(); ++i) {
        std::memcpy(code_base + blobs[i].offset, blobs[i].data.data(), blobs[i].data.size());
        DkShaderMaker shader_maker;
        dkShaderMakerDefaults(&shader_maker, code_memblock, blobs[i].offset);
        dkShaderInitialize(&specs[i].shader, &shader_maker);
    }
    return true;
}

void RendererDeko3D::SetupSampler() {
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    sampler.minFilter = DkFilter_Linear;
    sampler.magFilter = DkFilter_Linear;
    sampler.wrapMode[0] = DkWrapMode_ClampToEdge;
    sampler.wrapMode[1] = DkWrapMode_ClampToEdge;
    sampler.wrapMode[2] = DkWrapMode_ClampToEdge;

    DkSamplerDescriptor descriptor;
    dkSamplerDescriptorInitialize(&descriptor, &sampler);

    dkCmdBufClear(cmdbuf);
    dkCmdBufAddMemory(cmdbuf, cmdbuf_memblock, 0, CmdBufMemorySize);
    dkCmdBufPushData(cmdbuf, sampler_descriptor_set + SamplerSlot * sizeof(DkSamplerDescriptor),
                     &descriptor, sizeof(descriptor));
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
    dkQueueWaitIdle(queue);
    dkCmdBufClear(cmdbuf);
}

void RendererDeko3D::ConfigureScreenTexture(ScreenTexture& screen, u32 width, u32 height) {
    if (screen.valid && screen.width == width && screen.height == height) {
        return;
    }
    if (screen.memblock != nullptr) {
        dkMemBlockDestroy(screen.memblock);
        screen.memblock = nullptr;
    }

    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, device);
    layout_maker.format = DkImageFormat_RGBA8_Unorm;
    layout_maker.dimensions[0] = width;
    layout_maker.dimensions[1] = height;

    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &layout_maker);

    const u32 image_size = AlignUp(static_cast<u32>(dkImageLayoutGetSize(&layout)),
                                   dkImageLayoutGetAlignment(&layout));
    const u32 block_size = AlignUp(image_size, DK_MEMBLOCK_ALIGNMENT);

    DkMemBlockMaker memblock_maker;
    dkMemBlockMakerDefaults(&memblock_maker, device, block_size);
    memblock_maker.flags = DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image;
    screen.memblock = dkMemBlockCreate(&memblock_maker);

    dkImageInitialize(&screen.image, &layout, screen.memblock, 0);

    DkImageView view;
    dkImageViewDefaults(&view, &screen.image);
    dkImageDescriptorInitialize(&screen.descriptor, &view, false, false);

    screen.width = width;
    screen.height = height;
    screen.valid = true;
}

void RendererDeko3D::UploadScreen(ScreenTexture& screen,
                                  const Pica::FramebufferConfig& framebuffer, PAddr framebuffer_addr,
                                  const Pica::ColorFill& color_fill) {
    u32 width = framebuffer.width;
    u32 height = framebuffer.height;

    if (color_fill.is_enabled) {
        width = 1;
        height = 1;
    }
    if (width == 0 || height == 0) {
        screen.valid = false;
        return;
    }

    ConfigureScreenTexture(screen, width, height);

    const u32 dst_pitch = width * 4;
    const u32 upload_size = dst_pitch * height;
    staging_offset = AlignUp(staging_offset, StagingAlignment);
    if (staging_offset + upload_size > StagingMemorySize) {
        staging_offset = 0;
    }
    u8* const staging = static_cast<u8*>(dkMemBlockGetCpuAddr(staging_memblock)) + staging_offset;
    const DkGpuAddr staging_addr = dkMemBlockGetGpuAddr(staging_memblock) + staging_offset;
    staging_offset += upload_size;

    if (color_fill.is_enabled) {
        const Common::Vec3<u8> fill = color_fill.AsVector();
        staging[0] = fill.r();
        staging[1] = fill.g();
        staging[2] = fill.b();
        staging[3] = 255;
    } else {
        // Lets the rasterizer flush GPU-rendered output first.
        rasterizer.FlushRegion(framebuffer_addr, framebuffer.stride * height);
        const u8* fb_data = system.Memory().GetPhysicalPointer(framebuffer_addr);
        if (fb_data == nullptr) {
            screen.valid = false;
            return;
        }
        for (u32 y = 0; y < height; ++y) {
            DecodeRow(framebuffer.color_format, fb_data + y * framebuffer.stride,
                      staging + y * dst_pitch, width);
        }
    }

    DkImageView view;
    dkImageViewDefaults(&view, &screen.image);
    const DkImageRect rect = {0, 0, 0, width, height, 1};
    const DkCopyBuf copy_src = {staging_addr, 0, 0};
    dkCmdBufCopyBufferToImage(cmdbuf, &copy_src, &view, &rect, 0);
}

void RendererDeko3D::PrepareScreens() {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;

    // Top screen is framebuffer 0 bottom screen is framebuffer 1.
    const auto& top = framebuffer_config[0];
    const auto& bottom = framebuffer_config[1];

    const PAddr top_addr = top.active_fb == 0 ? top.address_left1 : top.address_left2;
    const PAddr bottom_addr = bottom.active_fb == 0 ? bottom.address_left1 : bottom.address_left2;

    UploadScreen(screens[0], top, top_addr, regs_lcd.color_fill_top);
    UploadScreen(screens[2], bottom, bottom_addr, regs_lcd.color_fill_bottom);
}

void RendererDeko3D::DrawScreenQuad(u32 image_slot, const Common::Rectangle<u32>& rect) {
    const auto ndc_x = [this](float px) { return px / static_cast<float>(fb_width) * 2.0f - 1.0f; };
    const auto ndc_y = [this](float py) { return 1.0f - py / static_cast<float>(fb_height) * 2.0f; };

    const float x0 = ndc_x(static_cast<float>(rect.left));
    const float x1 = ndc_x(static_cast<float>(rect.right));
    const float y0 = ndc_y(static_cast<float>(rect.top));
    const float y1 = ndc_y(static_cast<float>(rect.bottom));

    // Texcoords are transposed (U<->window-Y, V<->window-X).
    const PresentVertex vertices[4] = {
        {{x0, y0}, {1.0f, 0.0f}},
        {{x1, y0}, {1.0f, 1.0f}},
        {{x0, y1}, {0.0f, 0.0f}},
        {{x1, y1}, {0.0f, 1.0f}},
    };

    vertex_offset = AlignUp(vertex_offset, static_cast<u32>(sizeof(PresentVertex)));
    if (vertex_offset + sizeof(vertices) > VertexMemorySize) {
        vertex_offset = 0;
    }
    std::memcpy(static_cast<u8*>(dkMemBlockGetCpuAddr(vertex_memblock)) + vertex_offset, vertices,
                sizeof(vertices));
    const DkGpuAddr vertex_addr = dkMemBlockGetGpuAddr(vertex_memblock) + vertex_offset;
    vertex_offset += sizeof(vertices);

    static const DkVtxAttribState attribs[2] = {
        {0, 0, offsetof(PresentVertex, position), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(PresentVertex, tex_coord), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    };
    static const DkVtxBufferState buffer_state = {sizeof(PresentVertex), 0};

    const DkResHandle texture_handle = dkMakeTextureHandle(image_slot, SamplerSlot);
    dkCmdBufBindTextures(cmdbuf, DkStage_Fragment, 0, &texture_handle, 1);
    dkCmdBufBindVtxAttribState(cmdbuf, attribs, 2);
    dkCmdBufBindVtxBufferState(cmdbuf, &buffer_state, 1);
    dkCmdBufBindVtxBuffer(cmdbuf, 0, vertex_addr, sizeof(vertices));
    dkCmdBufDraw(cmdbuf, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

void RendererDeko3D::Present() {
    const int slot = dkQueueAcquireImage(queue, swapchain);

    DkImageView target_view;
    dkImageViewDefaults(&target_view, &framebuffers[slot]);
    dkCmdBufBindRenderTarget(cmdbuf, &target_view, nullptr);

    const DkViewport viewport = {
        0.0f, 0.0f, static_cast<float>(fb_width), static_cast<float>(fb_height), 0.0f, 1.0f};
    const DkScissor scissor = {0, 0, fb_width, fb_height};
    dkCmdBufSetViewports(cmdbuf, 0, &viewport, 1);
    dkCmdBufSetScissors(cmdbuf, 0, &scissor, 1);
    dkCmdBufClearColorFloat(cmdbuf, 0, DkColorMask_RGBA, ClearColor[0], ClearColor[1], ClearColor[2],
                            ClearColor[3]);

    const bool can_draw = shaders_ok && (screens[0].valid || screens[2].valid);
    if (can_draw) {
        // Make sure the uploads and descriptor writes are visible to the sampler before drawing.
        for (u32 i = 0; i < NumScreens; ++i) {
            if (screens[i].valid) {
                dkCmdBufPushData(cmdbuf, image_descriptor_set + i * sizeof(DkImageDescriptor),
                                 &screens[i].descriptor, sizeof(DkImageDescriptor));
            }
        }
        dkCmdBufBarrier(cmdbuf, DkBarrier_Full,
                        DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

        DkRasterizerState rasterizer_state;
        DkColorState color_state;
        DkColorWriteState color_write_state;
        dkRasterizerStateDefaults(&rasterizer_state);
        dkColorStateDefaults(&color_state);
        dkColorWriteStateDefaults(&color_write_state);
        rasterizer_state.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(cmdbuf, &rasterizer_state);
        dkCmdBufBindColorState(cmdbuf, &color_state);
        dkCmdBufBindColorWriteState(cmdbuf, &color_write_state);

        DkDepthStencilState depth_state;
        dkDepthStencilStateDefaults(&depth_state);
        depth_state.depthTestEnable = false;
        depth_state.depthWriteEnable = false;
        dkCmdBufBindDepthStencilState(cmdbuf, &depth_state);

        const DkShader* shaders[2] = {&present_vsh, &present_fsh};
        dkCmdBufBindShaders(cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
        dkCmdBufBindImageDescriptorSet(cmdbuf, image_descriptor_set, NumImageDescriptors);
        dkCmdBufBindSamplerDescriptorSet(cmdbuf, sampler_descriptor_set, NumSamplers);

        const auto& layout = render_window.GetFramebufferLayout();
        if (screens[0].valid && layout.top_screen_enabled) {
            DrawScreenQuad(0, layout.top_screen);
        }
        if (screens[2].valid && layout.bottom_screen_enabled) {
            DrawScreenQuad(2, layout.bottom_screen);
        }
    }

    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
    dkQueuePresentImage(queue, swapchain, slot);
    // TODO: Fences
    dkQueueWaitIdle(queue);
}

void RendererDeko3D::SwapBuffers() {
    if (!initialized) {
        EndFrame();
        return;
    }

    // Open a fresh command list for this frame.
    dkCmdBufClear(cmdbuf);
    dkCmdBufAddMemory(cmdbuf, cmdbuf_memblock, 0, CmdBufMemorySize);
    staging_offset = 0;
    vertex_offset = 0;

    PrepareScreens();
    Present();
    EndFrame();
}

void RendererDeko3D::TryPresent(int, bool) {
}

void RendererDeko3D::ExitDeko3D() {
    if (queue != nullptr) {
        dkQueueWaitIdle(queue);
    }
    for (auto& screen : screens) {
        if (screen.memblock != nullptr) {
            dkMemBlockDestroy(screen.memblock);
            screen.memblock = nullptr;
        }
    }
    if (queue != nullptr) {
        dkQueueDestroy(queue);
        queue = nullptr;
    }
    if (cmdbuf != nullptr) {
        dkCmdBufDestroy(cmdbuf);
        cmdbuf = nullptr;
    }
    if (vertex_memblock != nullptr) {
        dkMemBlockDestroy(vertex_memblock);
        vertex_memblock = nullptr;
    }
    if (staging_memblock != nullptr) {
        dkMemBlockDestroy(staging_memblock);
        staging_memblock = nullptr;
    }
    if (descriptor_memblock != nullptr) {
        dkMemBlockDestroy(descriptor_memblock);
        descriptor_memblock = nullptr;
    }
    if (code_memblock != nullptr) {
        dkMemBlockDestroy(code_memblock);
        code_memblock = nullptr;
    }
    if (cmdbuf_memblock != nullptr) {
        dkMemBlockDestroy(cmdbuf_memblock);
        cmdbuf_memblock = nullptr;
    }
    if (swapchain != nullptr) {
        dkSwapchainDestroy(swapchain);
        swapchain = nullptr;
    }
    if (fb_memblock != nullptr) {
        dkMemBlockDestroy(fb_memblock);
        fb_memblock = nullptr;
    }
    if (device != nullptr) {
        dkDeviceDestroy(device);
        device = nullptr;
    }
    initialized = false;
}

} // namespace Deko3D
