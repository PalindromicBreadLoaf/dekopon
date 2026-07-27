// Copyright 2023 Citra Emulator Project
// Copyright 2024 Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

#include "common/archives.h"
#include "common/microprofile.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "video_core/debug_utils/debug_utils.h"
#include "video_core/gpu.h"
#include "video_core/gpu_debugger.h"
#include "video_core/gpu_impl.h"
#include "video_core/gpu_thread.h"
#include "video_core/pica/pica_core.h"
#include "video_core/pica/regs_lcd.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_software/sw_blitter.h"
#include "video_core/right_eye_disabler.h"
#include "video_core/video_core.h"

namespace VideoCore {
struct GPU::Impl {
    struct PageTableUpdate {
        PAddr start;
        u32 size;
        bool cached;
    };

    Core::Timing& timing;
    Core::System& system;
    Memory::MemorySystem& memory;
    std::shared_ptr<Pica::DebugContext> debug_context;
    Pica::PicaCore pica;
    Pica::PicaCore::Regs shadow_regs{};
    Pica::RegsLcd shadow_regs_lcd{};
    GraphicsDebugger gpu_debugger;
    std::unique_ptr<RendererBase> renderer;
    RasterizerInterface* rasterizer;
    std::unique_ptr<SwRenderer::SwBlitter> sw_blitter;
    Core::TimingEventType* vblank_event;
    Core::TimingEventType* async_interrupt_event;
    Core::TimingEventType* page_table_update_event;
    Service::GSP::InterruptHandler signal_interrupt;
    std::unique_ptr<GPUThread> gpu_thread;
    std::mutex page_table_update_mutex;
    std::vector<PageTableUpdate> page_table_updates;
    std::vector<PageTableUpdate> page_table_update_scratch;

    /// Checked before the mutex because the drain sits on the CPU read path.
    std::atomic_bool has_page_table_updates{};
    std::atomic_bool page_table_update_event_scheduled{};
    u32 gpu_command_core{};
    s64 gpu_command_submit_tick{};
    u64 last_swap_fence{};

    /// Reused across calls so batching GSP register writes never allocates.
    std::vector<WriteRegCommand> reg_write_batch;

    // Invalidates arrive one per guest store, so neighbouring ones are merged into a single
    // queue entry. Flushed before anything else is queued, which keeps the ordering exact.
    PAddr pending_invalidate_start{};
    PAddr pending_invalidate_end{};
    bool has_pending_invalidate{};

#ifdef __SWITCH__
    u32 perf_stats_frame_counter{};
#endif
    bool async_gpu_enabled{};

    /// Resolved form of async_gpu_enabled plus every condition that forces synchronous operation.
    bool use_async{};
    bool strict_sync{};

    explicit Impl(Core::System& system, Frontend::EmuWindow& emu_window,
                  Frontend::EmuWindow* secondary_window)
        : timing{system.CoreTiming()}, system{system}, memory{system.Memory()},
          debug_context{Pica::g_debug_context}, pica{memory, debug_context},
          renderer{VideoCore::CreateRenderer(emu_window, secondary_window, pica, system)},
          rasterizer{renderer->Rasterizer()},
          sw_blitter{std::make_unique<SwRenderer::SwBlitter>(memory, rasterizer)} {
        shadow_regs = pica.regs;
        shadow_regs_lcd = pica.regs_lcd;
    }
    ~Impl();
};
} // namespace VideoCore
