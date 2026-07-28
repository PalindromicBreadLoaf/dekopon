// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <array>
#include <chrono>
#include <vector>

#include "common/common_types.h"
#include "common/math_util.h"
#include "video_core/renderer_base.h"
#ifdef HAVE_LIBRETRO
#include "citra_libretro/libretro_vk.h"
#else
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_present_window.h"
#endif
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_render_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Core {
class System;
}

namespace Memory {
class MemorySystem;
}

namespace Pica {
class PicaCore;
}

namespace Layout {
struct FramebufferLayout;
}

namespace VideoCore {
class GPU;
}

namespace Vulkan {

struct TextureInfo {
    u32 width;
    u32 height;
    Pica::PixelFormat format;
    vk::Image image;
    vk::ImageView image_view;
    VmaAllocation allocation;
};

struct ScreenInfo {
    TextureInfo texture;
    Common::Rectangle<f32> texcoords;
    vk::ImageView image_view;
};

struct PresentUniformData {
    std::array<f32, 4 * 4> modelview;
    Common::Vec4f i_resolution;
    Common::Vec4f o_resolution;
    int screen_id_l = 0;
    int screen_id_r = 0;
    int layer = 0;
    int reverse_interlaced = 0;
};
static_assert(sizeof(PresentUniformData) == 112,
              "PresentUniformData does not structure in shader!");

class RendererVulkan : public VideoCore::RendererBase {
    static constexpr std::size_t PRESENT_PIPELINES = 3;

public:
    explicit RendererVulkan(Core::System& system, Pica::PicaCore& pica, Frontend::EmuWindow& window,
                            Frontend::EmuWindow* secondary_window);
    ~RendererVulkan() override;

    [[nodiscard]] VideoCore::RasterizerInterface* Rasterizer() override {
        return &rasterizer;
    }

    void NotifySurfaceChanged(bool second) override;

    void SwapBuffers() override;
    void TryPresent(int timeout_ms, bool is_secondary) override {}

private:
    void ReloadPipeline(Settings::StereoRenderOption render_3d);
    void CompileShaders();
    void CreateOverlayFont();
    void BuildLayouts();
    void BuildPipelines();
    void ConfigureFramebufferTexture(TextureInfo& texture,
                                     const Pica::FramebufferConfig& framebuffer);
    void ConfigureRenderPipeline();
    std::array<bool, 3> GetPresentedScreens() const;
    void PrepareRendertarget();
    void RenderScreenshot();
    void RenderScreenshotWithStagingCopy();
    bool TryRenderScreenshotWithHostMemory();
    void PrepareDraw(Frame* frame, const Layout::FramebufferLayout& layout);
    void RenderToWindow(PresentWindow& window, const Layout::FramebufferLayout& layout,
                        bool flipped);

    void DrawScreens(Frame* frame, const Layout::FramebufferLayout& layout, bool flipped);
    void DrawBottomScreen(const Layout::FramebufferLayout& layout,
                          const Common::Rectangle<u32>& bottom_screen);

    void DrawTopScreen(const Layout::FramebufferLayout& layout,
                       const Common::Rectangle<u32>& top_screen);
    void DrawSingleScreen(u32 screen_id, float x, float y, float w, float h,
                          Layout::DisplayOrientation orientation);
    void DrawSingleScreenStereo(u32 screen_id_l, u32 screen_id_r, float x, float y, float w,
                                float h, Layout::DisplayOrientation orientation);

    void ApplySecondLayerOpacity(float alpha);

    void DrawCursor(const Layout::FramebufferLayout& layout);

    // On-screen overlay geometry.
    struct OverlayDraw {
        struct Batch {
            std::array<float, 4> color;
            u32 first; // Vertex offset from base_vertex.
            u32 count;
        };
        std::vector<Batch> batches;
        u32 base_vertex{};
    };

    OverlayDraw PrepareFpsOverlay(const Layout::FramebufferLayout& layout);

    OverlayDraw PrepareShaderNotice(const Layout::FramebufferLayout& layout);

    OverlayDraw PrepareToast(const Layout::FramebufferLayout& layout);

    OverlayDraw PrepareQuickMenu(const Layout::FramebufferLayout& layout);

    void RecordOverlay(OverlayDraw overlay);

    void LoadFBToScreenInfo(const Pica::FramebufferConfig& framebuffer, ScreenInfo& screen_info,
                            bool right_eye);
    void FillScreen(Common::Vec3<u8> color, const TextureInfo& texture);

private:
    Memory::MemorySystem& memory;
    Pica::PicaCore& pica;

#ifdef HAVE_LIBRETRO
    LibRetroVKInstance instance;
#else
    Instance instance;
#endif
    Scheduler scheduler;
    RenderManager renderpass_cache;
    PresentWindow main_present_window;
    StreamBuffer vertex_buffer;
    // Dedicated ring for overlay geometry.
    // This is imperitive to having the menu work and definitely didn't take me
    // multiple hours of debugging to figure out the menu was eating itself.
    // See the note in renderer_vulkan.cpp regarding why.
    StreamBuffer overlay_vertex_buffer;
    DescriptorUpdateQueue update_queue;
    RasterizerVulkan rasterizer;
    std::unique_ptr<PresentWindow> secondary_present_window_ptr;
    DescriptorHeap present_heap;
    vk::UniquePipelineLayout present_pipeline_layout;
    std::array<vk::Pipeline, PRESENT_PIPELINES> present_pipelines;
    std::array<vk::ShaderModule, PRESENT_PIPELINES> present_shaders;
    std::array<vk::Sampler, 2> present_samplers;
    vk::ShaderModule present_vertex_shader;
    u32 current_pipeline = 0;

    std::array<ScreenInfo, 3> screen_infos{};
    PresentUniformData draw_info{};
    vk::ClearColorValue clear_color{};

    vk::ShaderModule cursor_vertex_shader{};
    vk::ShaderModule cursor_fragment_shader{};
    vk::Pipeline cursor_pipeline{};
    vk::UniquePipelineLayout cursor_pipeline_layout{};

    // Text is drawn with a pre-baked font that lies in overlay_font.h/cpp
    vk::ShaderModule overlay_vertex_shader{};
    vk::ShaderModule overlay_fragment_shader{};
    vk::Pipeline overlay_pipeline{};
    vk::UniquePipelineLayout overlay_pipeline_layout{};
    vk::Image overlay_font_image{};
    VmaAllocation overlay_font_allocation{};
    vk::ImageView overlay_font_view{};
    vk::Sampler overlay_font_sampler{};
    vk::UniqueDescriptorSetLayout overlay_descriptor_layout{};
    vk::UniqueDescriptorPool overlay_descriptor_pool{};
    vk::DescriptorSet overlay_descriptor_set{};
    float overlay_game_fps = 0.0f;
    std::chrono::steady_clock::time_point overlay_last_update{};
    // Keeps the shader-compile notice on screen for a short tail after the last build so
    // one can actually read it.
    std::chrono::steady_clock::time_point shader_notice_until{};

    bool isSecondaryWindow = false;
    bool secondaryWindowEnabled = false;
    bool screenRendered = false;
};

} // namespace Vulkan
