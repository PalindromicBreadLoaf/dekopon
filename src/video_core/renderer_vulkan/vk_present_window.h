// Copyright 2023-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <span>
#include <queue>
#include "common/polyfill_thread.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"
#ifdef ENABLE_LSFG
#include "video_core/frame_generation.h"
#include "video_core/renderer_vulkan/vk_lsfg.h"
#endif

VK_DEFINE_HANDLE(VmaAllocation)

namespace Frontend {
class EmuWindow;
}

namespace Vulkan {

class Instance;
class Swapchain;
class Scheduler;
class RenderManager;

struct OverlayDraw {
    struct Batch {
        std::array<float, 4> color;
        u32 first;
        u32 count;
    };
    std::vector<Batch> batches;
    u32 base_vertex{};

    void Clear() {
        batches.clear();
        base_vertex = 0;
    }
};

constexpr u32 kOverlayCount = 4;

#ifdef ENABLE_LSFG

constexpr u64 kOverlayBufferSize = 256 * 1024;

class OverlayRecorder {
public:
    virtual ~OverlayRecorder() = default;

    virtual void RecordOverlays(vk::CommandBuffer cmdbuf, vk::Buffer vertex_buffer,
                                std::span<const OverlayDraw> overlays) = 0;
};
#endif

struct Frame {
    u32 width;
    u32 height;
    u32 present_width;
    u32 present_height;
    VmaAllocation allocation;
    vk::Framebuffer framebuffer;
    vk::Image image;
    vk::ImageView image_view;
    vk::Semaphore render_ready;
    vk::Fence present_done;
    vk::CommandBuffer cmdbuf;
#ifdef ENABLE_LSFG
    std::array<vk::CommandBuffer, kMaxGeneratedFrames> generated_cmdbufs;
    VideoCore::FrameGenerationDecision frame_gen;
    std::array<OverlayDraw, kOverlayCount> overlays;
    bool overlays_deferred;
    VmaAllocation overlay_allocation;
    vk::Buffer overlay_buffer;
    u8* overlay_data;
    u64 overlay_offset;
#endif
};

class PresentWindow final {
public:
    explicit PresentWindow(Frontend::EmuWindow& emu_window, const Instance& instance,
                           Scheduler& scheduler, bool low_refresh_rate);
    ~PresentWindow();

    /// Waits for all queued frames to finish presenting.
    void WaitPresent();

    /// Returns the last used render frame.
    Frame* GetRenderFrame();

    /// Recreates the render frame to match provided parameters.
    void RecreateFrame(Frame* frame, u32 width, u32 height);

    /// Queues the provided frame for presentation.
    void Present(Frame* frame);

#ifdef ENABLE_LSFG
    [[nodiscard]] VideoCore::FrameGenerationDecision ClassifyFrameGeneration();

    void SetOverlayRecorder(OverlayRecorder* recorder) {
        overlay_recorder = recorder;
    }
#endif

    /// This is called to notify the rendering backend of a surface change
    void NotifySurfaceChanged();

    [[nodiscard]] vk::RenderPass Renderpass() const noexcept {
        return present_renderpass;
    }

    u32 ImageCount() const noexcept {
        return swapchain.GetImageCount();
    }

    vk::Format GetSurfaceFormat() const noexcept {
        return swapchain.GetSurfaceFormat().format;
    }

    bool CanScalePresent() const noexcept {
        return blit_supported;
    }

private:
    void PresentThread(std::stop_token token);

    void RecreateSwapchain(u32 width, u32 height);

    void AcquireSwapchainImage(u32 width, u32 height);

    struct BlitSource {
        vk::Image image;
        u32 width;
        u32 height;
        vk::ImageLayout layout;
        vk::AccessFlags access;
        vk::PipelineStageFlags stage;
    };

    void RecordBlitToSwapchain(vk::CommandBuffer cmdbuf, const BlitSource& source,
                               vk::Image swapchain_image, bool overlay_follows);

    void SubmitAndPresent(vk::CommandBuffer cmdbuf, vk::Semaphore render_ready, vk::Fence fence);

    void CopyToSwapchain(Frame* frame);

#ifdef ENABLE_LSFG
    void CreateOverlayBuffer(Frame& frame);

    vk::RenderPass CreateOverlayRenderpass();

    void RecreateOverlayTargets();

    void RecordOverlayPass(vk::CommandBuffer cmdbuf, const Frame* frame);

    void ResetFrameGeneration();

    void UpdateFrameGeneration(Frame* frame);

    bool CopyToSwapchainGenerated(Frame* frame);
#endif

    vk::RenderPass CreateRenderpass();

private:
    Frontend::EmuWindow& emu_window;
    const Instance& instance;
    Scheduler& scheduler;
    bool low_refresh_rate;
    vk::SurfaceKHR surface;
    vk::SurfaceKHR next_surface{};
    Swapchain swapchain;
    vk::CommandPool command_pool;
    vk::Queue graphics_queue;
    vk::RenderPass present_renderpass;
    std::vector<Frame> swap_chain;
    std::queue<Frame*> free_queue;
    std::queue<Frame*> present_queue;
    std::condition_variable free_cv;
    std::condition_variable recreate_surface_cv;
    std::condition_variable_any frame_cv;
    std::mutex swapchain_mutex;
    std::mutex recreate_surface_mutex;
    std::mutex queue_mutex;
    std::mutex free_mutex;
    std::jthread present_thread;
    bool vsync_enabled{};
    bool blit_supported;
    bool async_presentation{true};
    bool use_present_thread{true};
    void* last_render_surface{};
#ifdef ENABLE_LSFG
    LsfgBridgePtr lsfg_bridge;
    struct LsfgConfig {
        u32 width;
        u32 height;
        u32 multiplier;
        u32 flow_scale;
        bool performance_mode;

        bool operator==(const LsfgConfig&) const = default;
    };
    LsfgConfig lsfg_config{};
    bool lsfg_attempted{};
    std::atomic<bool> lsfg_unavailable{};
    OverlayRecorder* overlay_recorder{};
    vk::RenderPass overlay_renderpass;
    std::vector<vk::ImageView> overlay_views;
    std::vector<vk::Framebuffer> overlay_framebuffers;
#endif
};

} // namespace Vulkan
