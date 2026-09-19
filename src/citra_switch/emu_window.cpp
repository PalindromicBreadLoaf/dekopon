// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#ifdef ENABLE_OPENGL
#include <glad/glad.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#endif

#include "citra_switch/config.h"
#include "citra_switch/emu_window.h"
#include "citra_switch/input.h"
#include "common/assert.h"
#include "common/horizon_display.h"
#include "common/logging/log.h"
#include "common/settings.h"

namespace {

constexpr int kSwitchScreenWidth = static_cast<int>(Common::Horizon::HandheldWidth);
constexpr int kSwitchScreenHeight = static_cast<int>(Common::Horizon::HandheldHeight);

constexpr std::uint64_t PackSize(unsigned width, unsigned height) {
    return (std::uint64_t{width} << 32) | height;
}

#ifdef ENABLE_OPENGL
// Switch's mesa/nouveau EGL exposes configs as EGL_OPENGL_ES2_BIT despite supporting GLES3
constexpr std::array<EGLint, 17> config_attribs{
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_DEPTH_SIZE,      0,
    EGL_STENCIL_SIZE,    0,
    EGL_NONE,
};
// Fallback used only if the combined window+pbuffer request matches nothing.
constexpr std::array<EGLint, 15> config_attribs_no_pbuffer{
    EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
    EGL_RED_SIZE,        8,
    EGL_GREEN_SIZE,      8,
    EGL_BLUE_SIZE,       8,
    EGL_ALPHA_SIZE,      8,
    EGL_NONE,
};
constexpr std::array<EGLint, 3> context_attribs{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
constexpr std::array<EGLint, 5> pbuffer_attribs{EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};

// Background work shared context
class SharedContext_Switch : public Frontend::GraphicsContext {
public:
    SharedContext_Switch(EGLDisplay display, EGLConfig config, EGLContext share)
        : egl_display{display},
          egl_surface{eglCreatePbufferSurface(display, config, pbuffer_attribs.data())},
          egl_context{eglCreateContext(display, config, share, context_attribs.data())} {
        if (egl_surface == EGL_NO_SURFACE) {
            LOG_WARNING(Frontend,
                        "eglCreatePbufferSurface() failed (0x{:x})",
                        eglGetError());
        }
        ASSERT_MSG(egl_context != EGL_NO_CONTEXT, "eglCreateContext() failed: 0x{:x}",
                   eglGetError());
    }

    ~SharedContext_Switch() override {
        eglDestroySurface(egl_display, egl_surface);
        eglDestroyContext(egl_display, egl_context);
    }

    void MakeCurrent() override {
        if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
            LOG_CRITICAL(Frontend, "Shared-context eglMakeCurrent() failed: 0x{:x}", eglGetError());
        }
    }

    void DoneCurrent() override {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    bool IsGLES() override {
        return true;
    }

private:
    EGLDisplay egl_display{EGL_NO_DISPLAY};
    EGLSurface egl_surface{EGL_NO_SURFACE};
    EGLContext egl_context{EGL_NO_CONTEXT};
};
#endif // ENABLE_OPENGL

} // namespace

EmuWindow_Switch::EmuWindow_Switch(void* native_window, bool use_egl, bool is_secondary)
    : EmuWindow{is_secondary}, egl_enabled{use_egl} {
#ifdef ENABLE_OPENGL
    if (egl_enabled) {
        if (!CreateEGLContext(native_window)) {
            DestroyEGLContext();
            return;
        }
    } else
#endif
    {
        const bool docked = SwitchFrontend::DisplayFollowsDockState() && Common::Horizon::IsDocked();
        window_width = static_cast<int>(docked ? Common::Horizon::DockedWidth
                                               : Common::Horizon::HandheldWidth);
        window_height = static_cast<int>(docked ? Common::Horizon::DockedHeight
                                                : Common::Horizon::HandheldHeight);
        Common::Horizon::SetNativeWindowSize(native_window, window_width, window_height);
        window_info.type = Frontend::WindowSystemType::Switch;
        LOG_INFO(Frontend, "EmuWindow provided via Vulkan: {}x{} ({})", window_width, window_height,
                 docked ? "docked" : "handheld");
    }

    window_info.render_surface = native_window;

    requested_size.store(PackSize(window_width, window_height), std::memory_order_relaxed);
    UpdateCurrentFramebufferLayout(window_width, window_height);
    is_valid = true;
}

EmuWindow_Switch::~EmuWindow_Switch() {
#ifdef ENABLE_OPENGL
    DestroyEGLContext();
#endif
}

#ifdef ENABLE_OPENGL
bool EmuWindow_Switch::CreateEGLContext(void* native_window) {
    egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (egl_display == EGL_NO_DISPLAY) {
        LOG_CRITICAL(Frontend, "eglGetDisplay() failed: 0x{:x}", eglGetError());
        return false;
    }
    if (eglInitialize(egl_display, nullptr, nullptr) != EGL_TRUE) {
        LOG_CRITICAL(Frontend, "eglInitialize() failed: 0x{:x}", eglGetError());
        return false;
    }
    if (eglBindAPI(EGL_OPENGL_ES_API) != EGL_TRUE) {
        LOG_CRITICAL(Frontend, "eglBindAPI() failed: 0x{:x}", eglGetError());
        return false;
    }

    EGLint num_configs = 0;
    if (eglChooseConfig(egl_display, config_attribs.data(), &egl_config, 1, &num_configs) !=
            EGL_TRUE ||
        num_configs < 1) {
        // Fall back to a window-only config
        if (eglChooseConfig(egl_display, config_attribs_no_pbuffer.data(), &egl_config, 1,
                            &num_configs) != EGL_TRUE ||
            num_configs < 1) {
            EGLint total = 0;
            eglGetConfigs(egl_display, nullptr, 0, &total);
            LOG_CRITICAL(Frontend,
                         "eglChooseConfig() matched no configs (err 0x{:x}, {} configs available)",
                         eglGetError(), total);
            return false;
        }
    }

    egl_surface = eglCreateWindowSurface(egl_display, egl_config, native_window, nullptr);
    if (egl_surface == EGL_NO_SURFACE) {
        LOG_CRITICAL(Frontend, "eglCreateWindowSurface() failed: 0x{:x}", eglGetError());
        return false;
    }
    eglQuerySurface(egl_display, egl_surface, EGL_WIDTH, &window_width);
    eglQuerySurface(egl_display, egl_surface, EGL_HEIGHT, &window_height);
    if (window_width <= 0 || window_height <= 0) {
        window_width = kSwitchScreenWidth;
        window_height = kSwitchScreenHeight;
    }

    egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attribs.data());
    if (egl_context == EGL_NO_CONTEXT) {
        LOG_CRITICAL(Frontend, "eglCreateContext() failed: 0x{:x}", eglGetError());
        return false;
    }
    if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
        LOG_CRITICAL(Frontend, "eglMakeCurrent() failed: 0x{:x}", eglGetError());
        return false;
    }
    if (!gladLoadGLES2Loader(reinterpret_cast<GLADloadproc>(eglGetProcAddress))) {
        LOG_CRITICAL(Frontend, "gladLoadGLES2Loader() failed");
        return false;
    }
    eglSwapInterval(egl_display, Settings::values.use_vsync.GetValue() ? 1 : 0);
    glViewport(0, 0, window_width, window_height);

    LOG_INFO(Frontend, "EGL/GLES context up: {}x{}, GL_VERSION={}", window_width, window_height,
             reinterpret_cast<const char*>(glGetString(GL_VERSION)));
    return true;
}

void EmuWindow_Switch::DestroyEGLContext() {
    if (egl_display == EGL_NO_DISPLAY) {
        return;
    }
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_context != EGL_NO_CONTEXT) {
        eglDestroyContext(egl_display, egl_context);
        egl_context = EGL_NO_CONTEXT;
    }
    if (egl_surface != EGL_NO_SURFACE) {
        eglDestroySurface(egl_display, egl_surface);
        egl_surface = EGL_NO_SURFACE;
    }
    eglTerminate(egl_display);
    egl_display = EGL_NO_DISPLAY;
}
#endif // ENABLE_OPENGL

void EmuWindow_Switch::MakeCurrent() {
    if (!egl_enabled) {
        return; // Vulkan/software have no host GL context to bind.
    }
#ifdef ENABLE_OPENGL
    if (eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context) != EGL_TRUE) {
        LOG_CRITICAL(Frontend, "Window context eglMakeCurrent() failed: 0x{:x}", eglGetError());
    }
#endif
}

void EmuWindow_Switch::DoneCurrent() {
    if (!egl_enabled) {
        return;
    }
#ifdef ENABLE_OPENGL
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#endif
}

void EmuWindow_Switch::PollEvents() {
    // main thread handles this
}

void EmuWindow_Switch::RequestSize(unsigned width, unsigned height) {
    requested_size.store(PackSize(width, height), std::memory_order_relaxed);
}

std::pair<u32, u32> EmuWindow_Switch::GetTargetFramebufferSize() const {
    const std::uint64_t packed = requested_size.load(std::memory_order_relaxed);
    return {static_cast<u32>(packed >> 32), static_cast<u32>(packed & 0xFFFFFFFF)};
}

void EmuWindow_Switch::SwapBuffers() {
    if (!egl_enabled) {
        return; // the Vulkan renderer presents through its own swapchain.
    }
#ifdef ENABLE_OPENGL
    eglSwapInterval(egl_display, Settings::values.use_vsync.GetValue() ? 1 : 0);
    eglSwapBuffers(egl_display, egl_surface);
#endif
}

std::unique_ptr<Frontend::GraphicsContext> EmuWindow_Switch::CreateSharedContext() const {
    if (!egl_enabled) {
        // This is a fancy no-op
        return std::make_unique<Frontend::GraphicsContext>();
    }
#ifdef ENABLE_OPENGL
    return std::make_unique<SharedContext_Switch>(egl_display, egl_config, egl_context);
#else
    return std::make_unique<Frontend::GraphicsContext>();
#endif
}

Frontend::EmuWindow::CursorInfo EmuWindow_Switch::GetCursorInfo() const {
    const SwitchFrontend::PointerCursor pointer = SwitchFrontend::GetPointerCursor();
    if (!pointer.active) {
        return {};
    }
    // Prevent the virtual pointer from leaving the bottom screen area by clamping to [0, 1].
    const auto& bottom = GetFramebufferLayout().bottom_screen;
    return {true, pointer.frac_x * bottom.GetWidth(), pointer.frac_y * bottom.GetHeight()};
}

void EmuWindow_Switch::PresentClear() {
    if (!is_valid || !egl_enabled) {
        return;
    }
#ifdef ENABLE_OPENGL
    eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glClearColor(0.96f, 0.55f, 0.13f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SwapBuffers();
#endif
}

namespace {
std::unique_ptr<EmuWindow_Switch> s_window;
int s_requested_docked = -1;
} // namespace

EmuWindow_Switch* GetEmuWindow() {
    return s_window.get();
}

namespace SwitchFrontend {

bool DisplayFollowsDockState() {
    return Settings::GetWorkingGraphicsAPI() == Settings::GraphicsAPI::Vulkan;
}

bool CreateWindow(void* native_window) {
    // Only the GLES renderer needs a host EGL context; Vulkan (NXVK) and software
    // own the nwindow directly.
#ifdef ENABLE_OPENGL
    const bool use_egl = Settings::GetWorkingGraphicsAPI() == Settings::GraphicsAPI::OpenGL;
#else
    const bool use_egl = false;
#endif
    s_window = std::make_unique<EmuWindow_Switch>(native_window, use_egl);
    if (!s_window->IsValid()) {
        LOG_CRITICAL(Frontend, "Failed to bring up EmuWindow");
        s_window.reset();
        return false;
    }
    s_requested_docked = -1;
    return true;
}

void UpdateDisplayMode() {
    if (!s_window || !DisplayFollowsDockState()) {
        return;
    }
    const int docked = Common::Horizon::IsDocked() ? 1 : 0;
    if (docked == s_requested_docked) {
        return;
    }
    s_window->RequestSize(docked ? Common::Horizon::DockedWidth : Common::Horizon::HandheldWidth,
                          docked ? Common::Horizon::DockedHeight : Common::Horizon::HandheldHeight);
    if (RequestLayoutUpdate()) {
        s_requested_docked = docked;
    }
}

void ClearFrame() {
    if (s_window) {
        s_window->PresentClear();
    }
}

void DestroyWindow() {
    s_window.reset();
}

} // namespace SwitchFrontend
