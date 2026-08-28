// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include "common/math_util.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_deko3d/dk_common.h"
#include "video_core/renderer_deko3d/dk_rasterizer.h"
#include "video_core/renderer_deko3d/dk_texture_runtime.h"

namespace Core {
class System;
}

namespace Layout {
struct FramebufferLayout;
}

namespace Pica {
enum class PixelFormat : u32;
struct FramebufferConfig;
union ColorFill;
class PicaCore;
} // namespace Pica

namespace Deko3D {

/// Backing for one 3DS screen
struct ScreenTexture {
    DkMemBlock memblock = nullptr;
    DkImage image{};
    DkImageDescriptor descriptor{};
    u32 width = 0;
    u32 height = 0;
    bool valid = false;
};

/**
 * Native Deko3D backend
 * Currently has no rasterizer
 */
class RendererDeko3D : public VideoCore::RendererBase {
public:
    explicit RendererDeko3D(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window,
                            Frontend::EmuWindow* secondary_window);
    ~RendererDeko3D() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return rasterizer.get();
    }

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override;

    static constexpr unsigned NumFramebuffers = 2;
    // How many frames' worth of CPU-written GPU resources may be in flight at once.
    static constexpr unsigned NumFramesInFlight = 2;
    // screens[0] = top, [1] = bottom.
    static constexpr unsigned NumScreens = 2;

private:
    /// Per-frame GPU resources
    struct FrameContext {
        DkMemBlock cmdbuf_memblock = nullptr;
        DkCmdBuf cmdbuf = nullptr;

        DkMemBlock staging_memblock = nullptr;
        u32 staging_offset = 0;
        DkMemBlock vertex_memblock = nullptr;
        u32 vertex_offset = 0;

        DkMemBlock descriptor_memblock = nullptr;
        DkGpuAddr image_descriptor_set = 0;
        DkGpuAddr sampler_descriptor_set = 0;

        std::array<ScreenTexture, NumScreens> screens{};

        DkFence fence{};
        bool fence_pending = false;
    };

    /// Brings up the device/queue/swapchain/command-buffer/shaders.
    void InitDeko3D(void* native_window);
    /// Tears everything down in reverse order.
    void ExitDeko3D();
    /// (Re)creates the swapchain and its framebuffers at the given size.
    void CreateSwapchain(void* native_window, u32 width, u32 height);
    /// Destroys the swapchain and framebuffer memory.
    void DestroySwapchain();
    /// Reacts to a dock/undock by resizing the swapchain and the window layout.
    void UpdateDisplayMode();
    /// Loads the precompiled present shaders from romfs.
    bool LoadShaders();
    /// Records the immutable sampler descriptor into every frame context.
    void SetupSampler();

    /// Pulls the current framebuffers from guest memory into the screen textures.
    void PrepareScreens(FrameContext& frame);
    /// (Re)allocates a screen texture's backing image for the given dimensions.
    void ConfigureScreenTexture(ScreenTexture& screen, u32 width, u32 height);
    /// Decodes one framebuffer from guest memory and copies it into the screen image.
    void UploadScreen(FrameContext& frame, ScreenTexture& screen,
                      const Pica::FramebufferConfig& framebuffer, PAddr framebuffer_addr,
                      const Pica::ColorFill& color_fill);

    /// Acquires a swapchain image, draws the screens, submits and presents.
    void Present(FrameContext& frame);
    /// Records a single textured quad for one screen at its window rectangle.
    void DrawScreenQuad(FrameContext& frame, u32 image_slot, const Common::Rectangle<u32>& rect);

private:
    Pica::PicaCore& pica;
    std::unique_ptr<TextureRuntime> runtime;
    std::unique_ptr<RasterizerDeko3D> rasterizer;

    bool initialized = false;
    void* native_window = nullptr;
    u32 fb_width = 1280;
    u32 fb_height = 720;

    DkDevice device = nullptr;
    DkQueue queue = nullptr;

    DkMemBlock fb_memblock = nullptr;
    std::array<DkImage, NumFramebuffers> framebuffers{};
    DkSwapchain swapchain = nullptr;

    DkMemBlock code_memblock = nullptr;
    DkShader present_vsh{};
    DkShader present_fsh{};
    bool shaders_ok = false;

    std::array<FrameContext, NumFramesInFlight> frames{};
    u32 frame_index = 0;
};

} // namespace Deko3D
