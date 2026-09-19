// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#ifdef ENABLE_OPENGL
#include <EGL/egl.h>
#endif
#include "core/frontend/emu_window.h"

// EmuWindow backed by a native libnx nwindow.
class EmuWindow_Switch : public Frontend::EmuWindow {
public:
    explicit EmuWindow_Switch(void* native_window, bool use_egl = false, bool is_secondary = false);
    ~EmuWindow_Switch() override;

    void MakeCurrent() override;
    void DoneCurrent() override;
    void PollEvents() override;
    void SwapBuffers() override;
    std::unique_ptr<GraphicsContext> CreateSharedContext() const override;

    bool IsGLES() override {
        return true;
    }

    bool IsValid() const {
        return is_valid;
    }

    /// Clear the default framebuffer to a solid colour and present.
    void PresentClear();

    /// Reports the touch-pointer crosshair so the renderer can draw it on the bottom screen.
    CursorInfo GetCursorInfo() const override;

    void RequestSize(unsigned width, unsigned height);

    std::pair<u32, u32> GetTargetFramebufferSize() const override;

private:
#ifdef ENABLE_OPENGL
    bool CreateEGLContext(void* native_window);
    void DestroyEGLContext();

    EGLDisplay egl_display{EGL_NO_DISPLAY};
    EGLSurface egl_surface{EGL_NO_SURFACE};
    EGLContext egl_context{EGL_NO_CONTEXT};
    EGLConfig egl_config{};
#endif

    int window_width{};
    int window_height{};
    std::atomic<std::uint64_t> requested_size{};
    bool is_valid{};
    bool egl_enabled{}; // false without a host GL context (Vulkan/software)
};

/// Returns the live frontend window
EmuWindow_Switch* GetEmuWindow();
