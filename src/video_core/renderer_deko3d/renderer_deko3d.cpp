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
#include "video_core/renderer_deko3d/dk_display_switch.h"
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
    : VideoCore::RendererBase{system, window, secondary_window}, pica{pica_} {
    // Deko3D stores the opaque nwindow here.
    void* window_handle = window.GetWindowInfo().render_surface;
    if (window_handle == nullptr) {
        LOG_ERROR(Render, "Deko3d: no native window provided.");
        return;
    }
    InitDeko3D(window_handle);
}

RendererDeko3D::~RendererDeko3D() {
    ExitDeko3D();
}

void RendererDeko3D::InitDeko3D(void* window_handle) {
    native_window = window_handle;

    DkDeviceMaker device_maker;
    dkDeviceMakerDefaults(&device_maker);
    device = dkDeviceCreate(&device_maker);

    // Size the swapchain to the current dock state so both 720p and 1080p are native.
    const DisplayDimensions dims = QueryDisplayDimensions();
    fb_width = dims.width;
    fb_height = dims.height;
    CreateSwapchain(native_window, fb_width, fb_height);

    // Graphics queue
    DkQueueMaker queue_maker;
    dkQueueMakerDefaults(&queue_maker, device);
    queue_maker.flags = DkQueueFlags_Graphics;
    queue = dkQueueCreate(&queue_maker);

    DkMemBlockMaker memblock_maker;
    for (auto& frame : frames) {
        dkMemBlockMakerDefaults(&memblock_maker, device, CmdBufMemorySize);
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        frame.cmdbuf_memblock = dkMemBlockCreate(&memblock_maker);

        DkCmdBufMaker cmdbuf_maker;
        dkCmdBufMakerDefaults(&cmdbuf_maker, device);
        frame.cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
        dkCmdBufAddMemory(frame.cmdbuf, frame.cmdbuf_memblock, 0, CmdBufMemorySize);

        const u32 descriptor_size = AlignUp(
            NumImageDescriptors * static_cast<u32>(sizeof(DkImageDescriptor)) +
                NumSamplers * static_cast<u32>(sizeof(DkSamplerDescriptor)),
            DK_MEMBLOCK_ALIGNMENT);
        dkMemBlockMakerDefaults(&memblock_maker, device, descriptor_size);
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        frame.descriptor_memblock = dkMemBlockCreate(&memblock_maker);
        const DkGpuAddr descriptor_base = dkMemBlockGetGpuAddr(frame.descriptor_memblock);
        frame.image_descriptor_set = descriptor_base;
        frame.sampler_descriptor_set =
            descriptor_base + NumImageDescriptors * static_cast<u32>(sizeof(DkImageDescriptor));

        dkMemBlockMakerDefaults(&memblock_maker, device, StagingMemorySize);
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        frame.staging_memblock = dkMemBlockCreate(&memblock_maker);

        dkMemBlockMakerDefaults(&memblock_maker, device, VertexMemorySize);
        memblock_maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
        frame.vertex_memblock = dkMemBlockCreate(&memblock_maker);
    }

    shaders_ok = LoadShaders();
    if (!shaders_ok) {
        LOG_ERROR(Render, "deko3d: present shaders unavailable. Things will be broken.");
    }
    SetupSampler();

    runtime = std::make_unique<TextureRuntime>(device, queue);
    rasterizer = std::make_unique<RasterizerDeko3D>(system.Memory(), pica,
                                                    system.CustomTexManager(), *this, device, queue,
                                                    *runtime);

    render_window.UpdateCurrentFramebufferLayout(fb_width, fb_height);

    initialized = true;
    LOG_INFO(Render, "deko3d device initialized ({}x{}, {} framebuffers, shaders={})", fb_width,
             fb_height, NumFramebuffers, shaders_ok);

    UamSelfTest();
}

void RendererDeko3D::CreateSwapchain(void* window_handle, u32 width, u32 height) {
    DkImageLayoutMaker layout_maker;
    dkImageLayoutMakerDefaults(&layout_maker, device);
    layout_maker.flags =
        DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_HwCompression;
    layout_maker.format = DkImageFormat_RGBA8_Unorm;
    layout_maker.dimensions[0] = width;
    layout_maker.dimensions[1] = height;

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
    dkSwapchainMakerDefaults(&swapchain_maker, device, window_handle, swapchain_images.data(),
                             NumFramebuffers);
    swapchain = dkSwapchainCreate(&swapchain_maker);
}

void RendererDeko3D::DestroySwapchain() {
    if (swapchain != nullptr) {
        dkSwapchainDestroy(swapchain);
        swapchain = nullptr;
    }
    if (fb_memblock != nullptr) {
        dkMemBlockDestroy(fb_memblock);
        fb_memblock = nullptr;
    }
}

void RendererDeko3D::UpdateDisplayMode() {
    const DisplayDimensions dims = QueryDisplayDimensions();
    if (dims.width == fb_width && dims.height == fb_height) {
        return;
    }

    dkQueueWaitIdle(queue);
    for (auto& frame : frames) {
        frame.fence_pending = false;
    }
    DestroySwapchain();
    CreateSwapchain(native_window, dims.width, dims.height);
    fb_width = dims.width;
    fb_height = dims.height;
    render_window.UpdateCurrentFramebufferLayout(fb_width, fb_height);
    LOG_INFO(Render, "deko3d: display mode changed to {}x{}", fb_width, fb_height);
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

    DkCmdBuf cmdbuf = frames[0].cmdbuf;
    dkCmdBufClear(cmdbuf);
    dkCmdBufAddMemory(cmdbuf, frames[0].cmdbuf_memblock, 0, CmdBufMemorySize);
    for (auto& frame : frames) {
        dkCmdBufPushData(cmdbuf,
                         frame.sampler_descriptor_set + SamplerSlot * sizeof(DkSamplerDescriptor),
                         &descriptor, sizeof(descriptor));
    }
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

void RendererDeko3D::UploadScreen(FrameContext& frame, ScreenTexture& screen,
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
    frame.staging_offset = AlignUp(frame.staging_offset, StagingAlignment);
    if (frame.staging_offset + upload_size > StagingMemorySize) {
        frame.staging_offset = 0;
    }
    u8* const staging =
        static_cast<u8*>(dkMemBlockGetCpuAddr(frame.staging_memblock)) + frame.staging_offset;
    const DkGpuAddr staging_addr =
        dkMemBlockGetGpuAddr(frame.staging_memblock) + frame.staging_offset;
    frame.staging_offset += upload_size;

    if (color_fill.is_enabled) {
        const Common::Vec3<u8> fill = color_fill.AsVector();
        staging[0] = fill.r();
        staging[1] = fill.g();
        staging[2] = fill.b();
        staging[3] = 255;
    } else {
        // Lets the rasterizer flush GPU-rendered output first.
        rasterizer->FlushRegion(framebuffer_addr, framebuffer.stride * height);
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
    dkCmdBufCopyBufferToImage(frame.cmdbuf, &copy_src, &view, &rect, 0);
}

void RendererDeko3D::PrepareScreens(FrameContext& frame) {
    const auto& framebuffer_config = pica.regs.framebuffer_config;
    const auto& regs_lcd = pica.regs_lcd;

    const auto& top = framebuffer_config[0];
    const auto& bottom = framebuffer_config[1];

    const PAddr top_addr = top.active_fb == 0 ? top.address_left1 : top.address_left2;
    const PAddr bottom_addr = bottom.active_fb == 0 ? bottom.address_left1 : bottom.address_left2;

    UploadScreen(frame, frame.screens[0], top, top_addr, regs_lcd.color_fill_top);
    UploadScreen(frame, frame.screens[1], bottom, bottom_addr, regs_lcd.color_fill_bottom);
}

void RendererDeko3D::DrawScreenQuad(FrameContext& frame, u32 image_slot,
                                    const Common::Rectangle<u32>& rect) {
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

    frame.vertex_offset = AlignUp(frame.vertex_offset, static_cast<u32>(sizeof(PresentVertex)));
    if (frame.vertex_offset + sizeof(vertices) > VertexMemorySize) {
        frame.vertex_offset = 0;
    }
    std::memcpy(static_cast<u8*>(dkMemBlockGetCpuAddr(frame.vertex_memblock)) + frame.vertex_offset,
                vertices, sizeof(vertices));
    const DkGpuAddr vertex_addr =
        dkMemBlockGetGpuAddr(frame.vertex_memblock) + frame.vertex_offset;
    frame.vertex_offset += sizeof(vertices);

    static const DkVtxAttribState attribs[2] = {
        {0, 0, offsetof(PresentVertex, position), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
        {0, 0, offsetof(PresentVertex, tex_coord), DkVtxAttribSize_2x32, DkVtxAttribType_Float, 0},
    };
    static const DkVtxBufferState buffer_state = {sizeof(PresentVertex), 0};

    const DkResHandle texture_handle = dkMakeTextureHandle(image_slot, SamplerSlot);
    dkCmdBufBindTextures(frame.cmdbuf, DkStage_Fragment, 0, &texture_handle, 1);
    dkCmdBufBindVtxAttribState(frame.cmdbuf, attribs, 2);
    dkCmdBufBindVtxBufferState(frame.cmdbuf, &buffer_state, 1);
    dkCmdBufBindVtxBuffer(frame.cmdbuf, 0, vertex_addr, sizeof(vertices));
    dkCmdBufDraw(frame.cmdbuf, DkPrimitive_TriangleStrip, 4, 1, 0, 0);
}

void RendererDeko3D::Present(FrameContext& frame) {
    const int slot = dkQueueAcquireImage(queue, swapchain);

    DkImageView target_view;
    dkImageViewDefaults(&target_view, &framebuffers[slot]);
    dkCmdBufBindRenderTarget(frame.cmdbuf, &target_view, nullptr);

    const DkViewport viewport = {
        0.0f, 0.0f, static_cast<float>(fb_width), static_cast<float>(fb_height), 0.0f, 1.0f};
    const DkScissor scissor = {0, 0, fb_width, fb_height};
    dkCmdBufSetViewports(frame.cmdbuf, 0, &viewport, 1);
    dkCmdBufSetScissors(frame.cmdbuf, 0, &scissor, 1);
    dkCmdBufClearColorFloat(frame.cmdbuf, 0, DkColorMask_RGBA, 0.0f, 0.0f, 0.0f, 1.0f);

    const bool can_draw = shaders_ok && (frame.screens[0].valid || frame.screens[1].valid);
    if (can_draw) {
        // Make sure the uploads and descriptor writes are visible to the sampler before drawing.
        for (u32 i = 0; i < NumScreens; ++i) {
            if (frame.screens[i].valid) {
                dkCmdBufPushData(frame.cmdbuf,
                                 frame.image_descriptor_set + i * sizeof(DkImageDescriptor),
                                 &frame.screens[i].descriptor, sizeof(DkImageDescriptor));
            }
        }
        dkCmdBufBarrier(frame.cmdbuf, DkBarrier_Full,
                        DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

        DkRasterizerState rasterizer_state;
        DkColorState color_state;
        DkColorWriteState color_write_state;
        dkRasterizerStateDefaults(&rasterizer_state);
        dkColorStateDefaults(&color_state);
        dkColorWriteStateDefaults(&color_write_state);
        rasterizer_state.cullMode = DkFace_None;
        dkCmdBufBindRasterizerState(frame.cmdbuf, &rasterizer_state);
        dkCmdBufBindColorState(frame.cmdbuf, &color_state);
        dkCmdBufBindColorWriteState(frame.cmdbuf, &color_write_state);

        DkDepthStencilState depth_state;
        dkDepthStencilStateDefaults(&depth_state);
        depth_state.depthTestEnable = false;
        depth_state.depthWriteEnable = false;
        dkCmdBufBindDepthStencilState(frame.cmdbuf, &depth_state);

        const DkShader* shaders[2] = {&present_vsh, &present_fsh};
        dkCmdBufBindShaders(frame.cmdbuf, DkStageFlag_GraphicsMask, shaders, 2);
        dkCmdBufBindImageDescriptorSet(frame.cmdbuf, frame.image_descriptor_set,
                                       NumImageDescriptors);
        dkCmdBufBindSamplerDescriptorSet(frame.cmdbuf, frame.sampler_descriptor_set, NumSamplers);

        const auto& layout = render_window.GetFramebufferLayout();
        if (frame.screens[0].valid && layout.top_screen_enabled) {
            DrawScreenQuad(frame, 0, layout.top_screen);
        }
        if (frame.screens[1].valid && layout.bottom_screen_enabled) {
            DrawScreenQuad(frame, 1, layout.bottom_screen);
        }
    }

    dkQueueSubmitCommands(queue, dkCmdBufFinishList(frame.cmdbuf));
    // Signal so the next reuse of this context waits precisely on this frame's GPU work.
    dkQueueSignalFence(queue, &frame.fence, false);
    frame.fence_pending = true;
    dkQueuePresentImage(queue, swapchain, slot);
}

void RendererDeko3D::SwapBuffers() {
    if (!initialized) {
        EndFrame();
        return;
    }

    UpdateDisplayMode();

    if (rasterizer) {
        rasterizer->TickFrame();
    }

    frame_index = (frame_index + 1) % NumFramesInFlight;
    FrameContext& frame = frames[frame_index];
    if (frame.fence_pending) {
        dkFenceWait(&frame.fence, -1);
        frame.fence_pending = false;
    }

    // Open a fresh command list for this frame.
    dkCmdBufClear(frame.cmdbuf);
    dkCmdBufAddMemory(frame.cmdbuf, frame.cmdbuf_memblock, 0, CmdBufMemorySize);
    frame.staging_offset = 0;
    frame.vertex_offset = 0;

    PrepareScreens(frame);
    Present(frame);
    EndFrame();
}

void RendererDeko3D::TryPresent(int, bool) {
}

void RendererDeko3D::ExitDeko3D() {
    if (queue != nullptr) {
        dkQueueWaitIdle(queue);
    }
    rasterizer.reset();
    runtime.reset();
    for (auto& frame : frames) {
        for (auto& screen : frame.screens) {
            if (screen.memblock != nullptr) {
                dkMemBlockDestroy(screen.memblock);
                screen.memblock = nullptr;
            }
        }
        if (frame.cmdbuf != nullptr) {
            dkCmdBufDestroy(frame.cmdbuf);
            frame.cmdbuf = nullptr;
        }
        if (frame.vertex_memblock != nullptr) {
            dkMemBlockDestroy(frame.vertex_memblock);
            frame.vertex_memblock = nullptr;
        }
        if (frame.staging_memblock != nullptr) {
            dkMemBlockDestroy(frame.staging_memblock);
            frame.staging_memblock = nullptr;
        }
        if (frame.descriptor_memblock != nullptr) {
            dkMemBlockDestroy(frame.descriptor_memblock);
            frame.descriptor_memblock = nullptr;
        }
        if (frame.cmdbuf_memblock != nullptr) {
            dkMemBlockDestroy(frame.cmdbuf_memblock);
            frame.cmdbuf_memblock = nullptr;
        }
    }
    if (queue != nullptr) {
        dkQueueDestroy(queue);
        queue = nullptr;
    }
    if (code_memblock != nullptr) {
        dkMemBlockDestroy(code_memblock);
        code_memblock = nullptr;
    }
    DestroySwapchain();
    if (device != nullptr) {
        dkDeviceDestroy(device);
        device = nullptr;
    }
    initialized = false;
}

} // namespace Deko3D
