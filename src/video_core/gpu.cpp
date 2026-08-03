// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

#include "common/archives.h"
#include "common/hacks/hack_manager.h"
#include "common/microprofile.h"
#include "common/settings.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/hle/service/gsp/gsp_gpu.h"
#include "core/hle/service/plgldr/plgldr.h"
#include "core/loader/loader.h"
#include "core/memory.h"
#include "core/perf_stats.h"
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

constexpr VAddr VADDR_LCD = 0x1ED02000;
constexpr VAddr VADDR_GPU = 0x1EF00000;

class DelayGenerator {
private:
    DelayGenerator() = default;

    // Average transfer speed based on measurements taken from real
    // hardware. 4 different modes have been taken into consideration:
    // RAM -> RAM, RAM -> VRAM, VRAM -> RAM and VRAM -> VRAM.
    // Furthermore, measurements are split into DMA transfers and tex
    // copies. For simplicity, we will assume fills are as fast as
    // texture copies.

    static constexpr double mibps_to_ns_per_byte(double mib_per_sec) {
        return 1'000'000'000.0 / (mib_per_sec * 1024.0 * 1024.0);
    }

    static constexpr std::array<std::array<double, 4>, 2> speed_mibps = {
        {{
             190.0, // DMA RAMTORAM
             310.0, // DMA RAMTOVRAM
             380.0, // DMA VRAMTORAM
             380.0, // DMA VRAMTOVRAM
         },
         {
             450.0,  // TEX RAMTORAM
             3100.0, // TEX RAMTOVRAM
             5400.0, // TEX VRAMTORAM
             5400.0, // TEX VRAMTOVRAM
         }}};

public:
    enum class CopyMode {
        RAMTORAM,
        RAMTOVRAM,
        VRAMTORAM,
        VRAMTOVRAM,
    };

    static CopyMode GetCopyMode(bool input_vram, bool output_vram) {
        if (!input_vram && !output_vram) {
            return CopyMode::RAMTORAM;
        } else if (!input_vram && output_vram) {
            return CopyMode::RAMTOVRAM;
        } else if (input_vram && !output_vram) {
            return CopyMode::VRAMTORAM;
        } else {
            return CopyMode::VRAMTOVRAM;
        }
    }

    static u64 CalculateDelayNanoseconds(CopyMode mode, bool is_textre, size_t size) {
        double base_ns_per_byte =
            mibps_to_ns_per_byte(speed_mibps[is_textre][static_cast<u32>(mode)]);

        return static_cast<u64>(size * base_ns_per_byte);
    }
};

MICROPROFILE_DEFINE(GPU_DisplayTransfer, "GPU", "DisplayTransfer", MP_RGB(100, 100, 255));
MICROPROFILE_DEFINE(GPU_CmdlistProcessing, "GPU", "Cmdlist Processing", MP_RGB(100, 255, 100));

GPU::GPU(Core::System& system, Frontend::EmuWindow& emu_window,
         Frontend::EmuWindow* secondary_window)
    : right_eye_disabler{std::make_unique<RightEyeDisabler>(*this)},
      impl{std::make_unique<Impl>(system, emu_window, secondary_window)} {
    impl->vblank_event = impl->timing.RegisterEvent(
        "GPU::VBlankCallback",
        [this](uintptr_t user_data, s64 cycles_late) { VBlankCallback(user_data, cycles_late); });
    impl->async_interrupt_event = impl->timing.RegisterEvent(
        "GPU::AsyncInterruptCallback", [this](uintptr_t user_data, s64 cycles_late) {
            AsyncInterruptCallback(user_data, cycles_late);
        });
    impl->page_table_update_event = impl->timing.RegisterEvent(
        "GPU::PageTableUpdateCallback", [this](uintptr_t user_data, s64 cycles_late) {
            PageTableUpdateCallback(user_data, cycles_late);
        });
    impl->timing.ScheduleEvent(FRAME_TICKS, impl->vblank_event);

    // Bind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);

    Service::GSP::InterruptHandler pica_interrupt = [this](Service::GSP::InterruptId interrupt_id,
                                                           u64 delay_ns, u64) {
        if (IsOnAsyncGPUThread()) {
            SignalInterruptFromGPU(interrupt_id, delay_ns, impl->gpu_command_core);
        } else {
            impl->signal_interrupt(interrupt_id, delay_ns, 0);
        }
    };
    impl->pica.SetInterruptHandler(pica_interrupt);

    if (Settings::values.async_gpu_emulation.GetValue() &&
        Settings::GetWorkingGraphicsAPI() == Settings::GraphicsAPI::OpenGL) {
        LOG_WARNING(HW_GPU,
                    "Async GPU requested with OpenGL. Using the synchronous GPU path because "
                    "renderer resources were created on the primary context");
    } else if (Settings::values.async_gpu_emulation.GetValue()) {
        auto context = emu_window.CreateSharedContext();
        if (context) {
            impl->async_gpu_enabled = true;
            impl->gpu_thread = std::make_unique<GPUThread>(
                std::move(context), [this](const GPUCommand& command, std::span<const u8> payload) {
                    ProcessCommand(command, payload);
                });
            LOG_INFO(HW_GPU, "Async GPU thread enabled");
        } else {
            LOG_WARNING(HW_GPU,
                        "Async GPU requested, but the frontend cannot provide a shared context");
        }
    }

    RefreshAsyncMode();
}

GPU::~GPU() = default;

GPU::Impl::~Impl() = default;

PAddr GPU::VirtualToPhysicalAddress(VAddr addr) {
    if (addr == 0) {
        return 0;
    }

    if (addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END) {
        return addr - Memory::VRAM_VADDR + Memory::VRAM_PADDR;
    }
    if (addr >= Memory::LINEAR_HEAP_VADDR && addr <= Memory::LINEAR_HEAP_VADDR_END) {
        return addr - Memory::LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    if (addr >= Memory::NEW_LINEAR_HEAP_VADDR && addr <= Memory::NEW_LINEAR_HEAP_VADDR_END) {
        return addr - Memory::NEW_LINEAR_HEAP_VADDR + Memory::FCRAM_PADDR;
    }
    PAddr plg_fb_addr;
    if (addr >= Memory::PLUGIN_3GX_FB_VADDR && addr <= Memory::PLUGIN_3GX_FB_VADDR_END &&
        (plg_fb_addr = impl->system.Memory().Plugin3GXFramebufferAddress())) {
        return addr - Memory::PLUGIN_3GX_FB_VADDR + plg_fb_addr;
    }

    LOG_ERROR(HW_Memory, "Unknown virtual address @ 0x{:08X}", addr);
    return addr;
}

void GPU::SetInterruptHandler(Service::GSP::InterruptHandler handler) {
    impl->signal_interrupt = handler;
}

// The CPU is about to read what the GPU produced, so the queue has to drain either way.
void GPU::FlushRegion(PAddr addr, u32 size) {
    SyncGpuThread();
    impl->rasterizer->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(PAddr addr, u32 size) {
    if (!ShouldUseAsync()) {
        SyncGpuThread();
        impl->rasterizer->InvalidateRegion(addr, size);
        return;
    }

    // Adjacent stores to a cached page arrive as a stream of tiny invalidates. Merging them keeps
    // the ring from filling up with thousands of entries that all drop the same surface.
    constexpr u32 MergeGap = 64;
    constexpr u32 MergeLimit = 16 * 1024;
    const PAddr end = addr + size;
    if (impl->has_pending_invalidate) {
        const PAddr merged_start = std::min(impl->pending_invalidate_start, addr);
        const PAddr merged_end = std::max(impl->pending_invalidate_end, end);
        if (addr <= impl->pending_invalidate_end + MergeGap &&
            end + MergeGap >= impl->pending_invalidate_start &&
            merged_end - merged_start <= MergeLimit) {
            impl->pending_invalidate_start = merged_start;
            impl->pending_invalidate_end = merged_end;
            return;
        }
        FlushPendingInvalidate();
    }

    impl->pending_invalidate_start = addr;
    impl->pending_invalidate_end = end;
    impl->has_pending_invalidate = true;
    if (impl->strict_sync) {
        SyncGpuThread();
    }
}

void GPU::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    SyncGpuThread();
    impl->rasterizer->FlushAndInvalidateRegion(addr, size);
}

void GPU::ClearAll(bool flush) {
    SyncGpuThread();
    impl->rasterizer->ClearAll(flush);
}

void GPU::WaitIdle() {
    SyncGpuThread();
}

void GPU::SwapBuffers() {
    if (ShouldUseAsync()) {
        // Cap the pipeline at one frame in flight so input latency and queue depth stay bounded.
        impl->gpu_thread->WaitForFence(impl->last_swap_fence);
        impl->last_swap_fence = QueueAsync(SwapBuffersCommand{});
    } else {
        SyncGpuThread();
        impl->renderer->SwapBuffers();
    }
}

void GPU::UpdateCurrentFramebufferLayout(bool is_portrait_mode) {
    if (ShouldUseAsync()) {
        QueueAsync(UpdateFramebufferLayoutCommand{is_portrait_mode});
    } else {
        SyncGpuThread();
        impl->renderer->UpdateCurrentFramebufferLayout(is_portrait_mode);
    }
}

void GPU::SyncGpuThread() {
    if (!impl->gpu_thread || impl->gpu_thread->IsCurrent()) {
        return;
    }
    FlushPendingInvalidate();
    impl->gpu_thread->WaitIdle();
    DrainPageTableUpdates();
}

void GPU::FlushPendingInvalidate() {
    if (!impl->has_pending_invalidate) {
        return;
    }

    const PAddr addr = impl->pending_invalidate_start;
    const u32 size = impl->pending_invalidate_end - addr;
    impl->has_pending_invalidate = false;

    if (impl->gpu_thread->IsIdle()) {
        DrainPageTableUpdates();
        impl->rasterizer->InvalidateRegion(addr, size);
        return;
    }
    QueueAsync(InvalidateRegionCommand{addr, size});
}

void GPU::Execute(const Service::GSP::Command& command) {
    using Service::GSP::CommandId;
    auto& regs = impl->shadow_regs;

    switch (command.id) {
    case CommandId::RequestDma: {
        impl->system.Memory().RasterizerFlushVirtualRegion(
            command.dma_request.source_address, command.dma_request.size, Memory::FlushMode::Flush);
        impl->system.Memory().RasterizerFlushVirtualRegion(command.dma_request.dest_address,
                                                           command.dma_request.size,
                                                           Memory::FlushMode::Invalidate);

        // TODO(Subv): These memory accesses should not go through the application's memory mapping.
        // They should go through the GSP module's memory mapping.
        const auto process = impl->system.Kernel().GetCurrentProcess();
        impl->memory.CopyBlock(*process, command.dma_request.dest_address,
                               command.dma_request.source_address, command.dma_request.size);

        auto is_vram = [&](u32 addr) {
            return addr >= Memory::VRAM_VADDR && addr <= Memory::VRAM_VADDR_END;
        };

        u64 delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(is_vram(command.dma_request.source_address),
                                        is_vram(command.dma_request.dest_address)),
            false, command.dma_request.size);

        impl->signal_interrupt(Service::GSP::InterruptId::DMA, delay, 0);
        break;
    }
    case CommandId::SubmitCmdList: {
        auto& params = command.submit_gpu_cmdlist;
        auto& cmdbuffer = regs.internal.pipeline.command_buffer;

        // Write to the command buffer GPU registers
        cmdbuffer.addr[0].Assign(VirtualToPhysicalAddress(params.address) >> 3);
        cmdbuffer.size[0].Assign(params.size >> 3);
        cmdbuffer.trigger[0] = 1;

        // Trigger processing of the command list
        SubmitCmdList(0);
        break;
    }
    case CommandId::MemoryFill: {
        auto& params = command.memory_fill;
        auto& memfill = regs.memory_fill_config;

        // Write to the memory fill GPU registers.
        // If both buffers are set GSP dispatches PSC0 only.
        const bool has_both_bufs = params.start1 != 0 && params.start2 != 0;
        if (params.start1 != 0) {
            memfill[0].address_start = VirtualToPhysicalAddress(params.start1) >> 3;
            memfill[0].address_end = VirtualToPhysicalAddress(params.end1) >> 3;
            memfill[0].value_32bit = params.value1;
            memfill[0].control = params.control1;
            const u32 interrupt_index = has_both_bufs ? std::numeric_limits<u32>::max() : 0;
            if (ShouldUseAsync()) {
                QueueAsync(MemoryFillCommand{memfill[0], 0, interrupt_index});
            } else {
                SyncGpuThread();
                ProcessSync(MemoryFillCommand{memfill[0], 0, interrupt_index});
            }
            memfill[0].trigger.Assign(0);
        }
        if (params.start2 != 0) {
            memfill[1].address_start = VirtualToPhysicalAddress(params.start2) >> 3;
            memfill[1].address_end = VirtualToPhysicalAddress(params.end2) >> 3;
            memfill[1].value_32bit = params.value2;
            memfill[1].control = params.control2;
            const u32 interrupt_index = has_both_bufs ? 0 : 1;
            if (ShouldUseAsync()) {
                QueueAsync(MemoryFillCommand{memfill[1], 1, interrupt_index});
            } else {
                SyncGpuThread();
                ProcessSync(MemoryFillCommand{memfill[1], 1, interrupt_index});
            }
            memfill[1].trigger.Assign(0);
        }
        if (impl->strict_sync) {
            SyncGpuThread();
        }
        break;
    }
    case CommandId::DisplayTransfer: {
        auto& params = command.display_transfer;
        auto& display_transfer = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        display_transfer.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        display_transfer.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
        display_transfer.input_size = params.in_buffer_size;
        display_transfer.output_size = params.out_buffer_size;
        display_transfer.flags = params.flags;
        display_transfer.trigger.Assign(1);

        if (ShouldUseAsync()) {
            QueueAsync(MemoryTransferCommand{display_transfer});
            if (impl->strict_sync) {
                SyncGpuThread();
            }
        } else {
            SyncGpuThread();
            ProcessSync(MemoryTransferCommand{display_transfer});
        }
        display_transfer.trigger.Assign(0);
        break;
    }
    case CommandId::TextureCopy: {
        auto& params = command.texture_copy;
        auto& texture_copy = regs.display_transfer_config;

        // Write to the transfer engine GPU registers.
        texture_copy.input_address = VirtualToPhysicalAddress(params.in_buffer_address) >> 3;
        texture_copy.output_address = VirtualToPhysicalAddress(params.out_buffer_address) >> 3;
        texture_copy.texture_copy.size = params.size;
        texture_copy.texture_copy.input_size = params.in_width_gap;
        texture_copy.texture_copy.output_size = params.out_width_gap;
        texture_copy.flags = params.flags;
        texture_copy.trigger.Assign(1);

        if (ShouldUseAsync()) {
            QueueAsync(MemoryTransferCommand{texture_copy});
            if (impl->strict_sync) {
                SyncGpuThread();
            }
        } else {
            SyncGpuThread();
            ProcessSync(MemoryTransferCommand{texture_copy});
        }
        texture_copy.trigger.Assign(0);
        break;
    }
    case CommandId::CacheFlush: {
        // Rasterizer flushing handled elsewhere in CPU read/write and other GPU handlers
        // Use command.cache_flush.regions to implement this handler
        break;
    }
    default:
        LOG_ERROR(HW_GPU, "Unknown command {:#08X}", command.id.Value());
    }

    // Notify debugger that a GSP command was processed.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::GSPCommandProcessed, &command);
    }
}

void GPU::SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info) {
    const PAddr phys_address_left = VirtualToPhysicalAddress(info.address_left);
    const PAddr phys_address_right = VirtualToPhysicalAddress(info.address_right);

    auto& framebuffer = impl->shadow_regs.framebuffer_config[screen_id];
    if (info.active_fb == 0) {
        framebuffer.address_left1 = phys_address_left;
        framebuffer.address_right1 = phys_address_right;
    } else {
        framebuffer.address_left2 = phys_address_left;
        framebuffer.address_right2 = phys_address_right;
    }

    framebuffer.stride = info.stride;
    framebuffer.format = info.format;
    framebuffer.active_fb = info.shown_fb;

    if (screen_id == 0) {
        MicroProfileFlip();
        impl->system.perf_stats->EndGameFrame();
    } else {
        // A bottom-screen-only swap is still something new to present, but it is not a game frame.
        Core::PerfStats::game_frames_updated = true;
    }

    BufferSwapCommand command{screen_id,   phys_address_left, phys_address_right, info.active_fb,
                              info.stride, info.format,       info.shown_fb};
    if (ShouldUseAsync()) {
        QueueAsync(command);
        if (impl->strict_sync) {
            SyncGpuThread();
        }
    } else {
        SyncGpuThread();
        ProcessSync(command);
    }
}

void GPU::SetColorFill(const Pica::ColorFill& fill) {
    impl->shadow_regs_lcd.color_fill_top = fill;
    impl->shadow_regs_lcd.color_fill_bottom = fill;

    if (ShouldUseAsync()) {
        QueueAsync(ColorFillCommand{fill});
        if (impl->strict_sync) {
            SyncGpuThread();
        }
    } else {
        SyncGpuThread();
        ProcessSync(ColorFillCommand{fill});
    }
}

u32 GPU::ReadReg(VAddr addr) {
    SyncGpuThread();

    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        const u32 value = impl->pica.regs_lcd[index];
        impl->shadow_regs_lcd[index] = value;
        return value;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        const u32 value = impl->pica.regs.reg_array[index];
        impl->shadow_regs.reg_array[index] = value;
        return value;
    }
    default:
        UNREACHABLE_MSG("Read from unknown GPU address {:#08X}", addr);
    }
}

void GPU::WriteReg(VAddr addr, u32 data) {
    WriteRegWithMask(addr, data, ~u32{});
}

GPU::RegWriteInfo GPU::PrepareWriteReg(VAddr addr, u32 data, u32 mask) {
    RegWriteInfo info{};

    switch (addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 offset = addr - VADDR_LCD;
        const u32 index = offset / sizeof(u32);
        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::RegsLcd::NumIds());
        impl->shadow_regs_lcd[index] = (impl->shadow_regs_lcd[index] & ~mask) | (data & mask);
        break;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 offset = addr - VADDR_GPU;
        const u32 index = offset / sizeof(u32);

        ASSERT(addr % sizeof(u32) == 0);
        ASSERT(index < Pica::PicaCore::Regs::NUM_REGS);
        impl->shadow_regs.reg_array[index] =
            (impl->shadow_regs.reg_array[index] & ~mask) | (data & mask);

        if (index == GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[0]) ||
            index == GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[1])) {
            const u32 command_index = static_cast<u32>(
                index - GPU_REG_INDEX(internal.pipeline.command_buffer.trigger[0]));
            const auto& config = impl->shadow_regs.internal.pipeline.command_buffer;
            if (config.trigger[command_index]) {
                info.has_command_list = true;
                info.triggers_work = true;
                info.command_list_index = command_index;
                info.command_list_address = config.GetPhysicalAddress(command_index);
                info.command_list_size = config.GetSize(command_index);
                impl->shadow_regs.internal.pipeline.command_buffer.trigger[command_index] = 0;
            }
        } else if (index == GPU_REG_INDEX(memory_fill_config[0].trigger)) {
            impl->shadow_regs.memory_fill_config[0].trigger.Assign(0);
            info.triggers_work = true;
        } else if (index == GPU_REG_INDEX(memory_fill_config[1].trigger)) {
            impl->shadow_regs.memory_fill_config[1].trigger.Assign(0);
            info.triggers_work = true;
        } else if (index == GPU_REG_INDEX(display_transfer_config.trigger)) {
            impl->shadow_regs.display_transfer_config.trigger.Assign(0);
            info.triggers_work = true;
        }
        break;
    }
    default:
        UNREACHABLE_MSG("Write to unknown GPU address {:#08X}", addr);
    }

    return info;
}

void GPU::WriteRegWithMask(VAddr addr, u32 data, u32 mask) {
    const RegWriteInfo info = PrepareWriteReg(addr, data, mask);
    const WriteRegCommand write{addr, data, mask};

    if (ShouldUseAsync()) {
        QueueAsync(write);
    } else {
        SyncGpuThread();
        ProcessSync(write);
    }

    if (info.has_command_list) {
        DispatchCmdList(info.command_list_index, info.command_list_address, info.command_list_size);
    }
    if (info.triggers_work && impl->strict_sync) {
        SyncGpuThread();
    }
}

void GPU::WriteRegs(VAddr addr, std::span<const u8> data, std::span<const u8> masks) {
    ASSERT(data.size() % sizeof(u32) == 0);
    ASSERT(masks.empty() || masks.size() == data.size());
    if (data.empty()) {
        return;
    }

    auto& batch = impl->reg_write_batch;
    batch.clear();

    bool triggered = false;
    for (std::size_t offset = 0; offset < data.size(); offset += sizeof(u32), addr += sizeof(u32)) {
        u32 value;
        u32 mask = ~u32{};
        std::memcpy(&value, data.data() + offset, sizeof(value));
        if (!masks.empty()) {
            std::memcpy(&mask, masks.data() + offset, sizeof(mask));
        }

        const RegWriteInfo info = PrepareWriteReg(addr, value, mask);
        batch.push_back(WriteRegCommand{addr, value, mask});
        triggered |= info.triggers_work;

        if (info.has_command_list) {
            // The triggering write has to land before the list runs.
            FlushRegWriteBatch();
            DispatchCmdList(info.command_list_index, info.command_list_address,
                            info.command_list_size);
        }
    }
    FlushRegWriteBatch();

    if (triggered && impl->strict_sync) {
        SyncGpuThread();
    }
}

void GPU::FlushRegWriteBatch() {
    auto& batch = impl->reg_write_batch;
    if (batch.empty()) {
        return;
    }

    if (batch.size() == 1) {
        if (ShouldUseAsync()) {
            QueueAsync(batch.front());
        } else {
            SyncGpuThread();
            ProcessSync(batch.front());
        }
    } else {
        const std::span<const u8> payload{reinterpret_cast<const u8*>(batch.data()),
                                          batch.size() * sizeof(WriteRegCommand)};
        const WriteRegsCommand command{static_cast<u32>(batch.size())};
        if (ShouldUseAsync()) {
            QueueAsync(command, payload);
        } else {
            SyncGpuThread();
            ProcessSync(command, payload);
        }
    }
    batch.clear();
}

void GPU::DispatchCmdList(u32 index, PAddr address, u32 size) {
    const std::span<const u8> commands = GuestCommandList(address, size);
    if (ShouldUseAsync() && commands.size() <= GPUThread::PayloadCapacity) {
        QueueAsync(SubmitCmdListCommand{index, address}, commands);
        return;
    }
    SyncGpuThread();
    ProcessSync(SubmitCmdListCommand{index, address}, commands);
}

VideoCore::RendererBase& GPU::Renderer() {
    SyncGpuThread();
    return *impl->renderer;
}

Pica::PicaCore& GPU::PicaCore() {
    SyncGpuThread();
    return impl->pica;
}

const Pica::PicaCore& GPU::PicaCore() const {
    const_cast<GPU*>(this)->SyncGpuThread();
    return impl->pica;
}

GraphicsDebugger& GPU::Debugger() {
    return impl->gpu_debugger;
}

void GPU::ApplyPerProgramSettings(u64 program_ID) {
    auto hack = Common::Hacks::hack_manager.GetHack(
        Common::Hacks::HackType::ACCURATE_MULTIPLICATION, program_ID);
    bool use_accurate_mul = Settings::values.shaders_accurate_mul.GetValue();
    if (hack) {
        switch (hack->mode) {
        case Common::Hacks::HackAllowMode::DISALLOW:
            use_accurate_mul = false;
            break;
        case Common::Hacks::HackAllowMode::FORCE:
            use_accurate_mul = true;
            break;
        case Common::Hacks::HackAllowMode::ALLOW:
        default:
            break;
        }
    }
    RefreshAsyncMode();
    if (ShouldUseAsync()) {
        QueueAsync(AccurateMulCommand{use_accurate_mul});
    } else {
        SyncGpuThread();
        ProcessSync(AccurateMulCommand{use_accurate_mul});
    }
}

void GPU::SetRightEyeEnabled(bool enabled) {
    if (ShouldUseAsync()) {
        QueueAsync(RightEyeCommand{enabled});
    } else {
        SyncGpuThread();
        ProcessSync(RightEyeCommand{enabled});
    }
}

void GPU::SubmitCmdList(u32 index) {
    auto& config = impl->shadow_regs.internal.pipeline.command_buffer;
    if (!config.trigger[index]) {
        return;
    }

    const PAddr addr = config.GetPhysicalAddress(index);
    const u32 size = config.GetSize(index);
    config.trigger[index] = 0;

    DispatchCmdList(index, addr, size);
    if (impl->strict_sync) {
        SyncGpuThread();
    }
}

void GPU::SubmitCmdList(u32 index, PAddr address, std::span<const u8> commands) {
    auto& config = impl->pica.regs.internal.pipeline.command_buffer;
    if (!config.trigger[index]) {
        return;
    }

    MICROPROFILE_SCOPE(GPU_CmdlistProcessing);
    impl->pica.ProcessCmdList(address, commands,
                              !right_eye_disabler->ShouldAllowCmdQueueTrigger(address, commands));
    config.trigger[index] = 0;
}

void GPU::MemoryFill(u32 index, u32 intr_index) {
    // Check if a memory fill was triggered.
    auto& config = impl->pica.regs.memory_fill_config[index];
    if (!config.trigger) {
        return;
    }

    // Perform memory fill.
    if (!impl->rasterizer->AccelerateFill(config)) {
        impl->sw_blitter->MemoryFill(config);
    }

    // Treat fill as texture transfer from VRAM
    u64 delay = DelayGenerator::CalculateDelayNanoseconds(
        DelayGenerator::GetCopyMode(true, config.IsVRAM()), true,
        config.GetEndAddress() - config.GetStartAddress());

    // It seems that it won't signal interrupt if "address_start" is zero.
    // TODO: hwtest this
    if (config.GetStartAddress() != 0) {
        if (intr_index == 0) {
            if (IsOnAsyncGPUThread()) {
                SignalInterruptFromGPU(Service::GSP::InterruptId::PSC0, delay,
                                       impl->gpu_command_core);
            } else {
                impl->signal_interrupt(Service::GSP::InterruptId::PSC0, delay, 0);
            }
        } else if (intr_index == 1) {
            if (IsOnAsyncGPUThread()) {
                SignalInterruptFromGPU(Service::GSP::InterruptId::PSC1, delay,
                                       impl->gpu_command_core);
            } else {
                impl->signal_interrupt(Service::GSP::InterruptId::PSC1, delay, 0);
            }
        }
    }

    // Reset "trigger" flag and set the "finish" flag
    // This was confirmed to happen on hardware even if "address_start" is zero.
    config.trigger.Assign(0);
    config.finished.Assign(1);
}

void GPU::MemoryTransfer() {
    // Check if a transfer was triggered.
    auto& config = impl->pica.regs.display_transfer_config;
    if (!config.trigger.Value()) {
        return;
    }

    MICROPROFILE_SCOPE(GPU_DisplayTransfer);

    // Notify debugger about the display transfer.
    if (impl->debug_context) {
        impl->debug_context->OnEvent(Pica::DebugContext::Event::IncomingDisplayTransfer, nullptr);
    }

    u64 delay{};
    // Perform memory transfer
    if (config.is_texture_copy) {
        if (!impl->rasterizer->AccelerateTextureCopy(config)) {
            impl->sw_blitter->TextureCopy(config);
        }
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.texture_copy.size);
    } else {
        if (right_eye_disabler->ShouldAllowDisplayTransfer(config.GetPhysicalInputAddress(),
                                                           config.input_height)) {
            if (!impl->rasterizer->AccelerateDisplayTransfer(config)) {
                impl->sw_blitter->DisplayTransfer(config);
            }
        }
        delay = DelayGenerator::CalculateDelayNanoseconds(
            DelayGenerator::GetCopyMode(config.IsInputVRAM(), config.IsOutputVRAM()), true,
            config.input_width * config.input_height * BytesPerPixel(config.input_format));
    }

    // Complete transfer.
    config.trigger.Assign(0);
    if (IsOnAsyncGPUThread()) {
        SignalInterruptFromGPU(Service::GSP::InterruptId::PPF, delay, impl->gpu_command_core);
    } else {
        impl->signal_interrupt(Service::GSP::InterruptId::PPF, delay, 0);
    }
}

void GPU::VBlankCallback(std::uintptr_t user_data, s64 cycles_late) {
    RefreshAsyncMode();

    // Signal to GSP that GPU interrupt has occurred
    impl->signal_interrupt(Service::GSP::InterruptId::PDC0, 0, 0);
    impl->signal_interrupt(Service::GSP::InterruptId::PDC1, 0, 0);

    // Present renderered frame.
    SwapBuffers();
    impl->renderer->EndFrameOnEmulationThread();
#ifdef __SWITCH__
    if (++impl->perf_stats_frame_counter >= 30) {
        impl->system.GetAndResetPerfStats();
        impl->perf_stats_frame_counter = 0;
    }
#endif

    // Reschedule recurrent event
    impl->timing.ScheduleEvent(FRAME_TICKS - cycles_late, impl->vblank_event);
}

void GPU::ProcessWriteReg(const WriteRegCommand& data) {
    switch (data.addr & 0xFFFFF000) {
    case VADDR_LCD: {
        const u32 index = (data.addr - VADDR_LCD) / sizeof(u32);
        impl->pica.regs_lcd[index] =
            (impl->pica.regs_lcd[index] & ~data.mask) | (data.value & data.mask);
        break;
    }
    case VADDR_GPU:
    case VADDR_GPU + 0x1000: {
        const u32 index = (data.addr - VADDR_GPU) / sizeof(u32);
        impl->pica.regs.reg_array[index] =
            (impl->pica.regs.reg_array[index] & ~data.mask) | (data.value & data.mask);
        switch (index) {
        case GPU_REG_INDEX(memory_fill_config[0].trigger):
            MemoryFill(0, 0);
            break;
        case GPU_REG_INDEX(memory_fill_config[1].trigger):
            MemoryFill(1, 1);
            break;
        case GPU_REG_INDEX(display_transfer_config.trigger):
            MemoryTransfer();
            break;
        default:
            // Command list triggers arrive as their own command, carrying the snapshot.
            break;
        }
        break;
    }
    default:
        UNREACHABLE();
    }
}

void GPU::ProcessCommand(const GPUCommand& command, std::span<const u8> payload) {
    if (!IsOnAsyncGPUThread()) {
        DrainPageTableUpdates();
    }
    impl->gpu_command_core = command.core_id;
    impl->gpu_command_submit_tick = command.submit_tick;

    std::visit(
        [&](const auto& data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, StopCommand>) {
                UNREACHABLE();
            } else if constexpr (std::is_same_v<T, WriteRegCommand>) {
                ProcessWriteReg(data);
            } else if constexpr (std::is_same_v<T, WriteRegsCommand>) {
                for (u32 i = 0; i < data.count; ++i) {
                    WriteRegCommand write;
                    std::memcpy(&write, payload.data() + i * sizeof(write), sizeof(write));
                    ProcessWriteReg(write);
                }
            } else if constexpr (std::is_same_v<T, SubmitCmdListCommand>) {
                auto& config = impl->pica.regs.internal.pipeline.command_buffer;
                config.addr[data.index].Assign(data.address >> 3);
                config.size[data.index].Assign(static_cast<u32>(payload.size()) >> 3);
                config.trigger[data.index] = 1;
                SubmitCmdList(data.index, data.address, payload);
            } else if constexpr (std::is_same_v<T, MemoryFillCommand>) {
                impl->pica.regs.memory_fill_config[data.index] = data.config;
                MemoryFill(data.index, data.interrupt_index);
            } else if constexpr (std::is_same_v<T, MemoryTransferCommand>) {
                impl->pica.regs.display_transfer_config = data.config;
                MemoryTransfer();
            } else if constexpr (std::is_same_v<T, BufferSwapCommand>) {
                auto& framebuffer = impl->pica.regs.framebuffer_config[data.screen_id];
                if (data.active_fb == 0) {
                    framebuffer.address_left1 = data.address_left;
                    framebuffer.address_right1 = data.address_right;
                } else {
                    framebuffer.address_left2 = data.address_left;
                    framebuffer.address_right2 = data.address_right;
                }
                framebuffer.stride = data.stride;
                framebuffer.format = data.format;
                framebuffer.active_fb = data.shown_fb;
                if (impl->debug_context) {
                    impl->debug_context->OnEvent(Pica::DebugContext::Event::BufferSwapped, nullptr);
                }
                if (data.screen_id == 0) {
                    right_eye_disabler->ReportEndFrame();
                }
            } else if constexpr (std::is_same_v<T, ColorFillCommand>) {
                impl->pica.regs_lcd.color_fill_top = data.fill;
                impl->pica.regs_lcd.color_fill_bottom = data.fill;
            } else if constexpr (std::is_same_v<T, InvalidateRegionCommand>) {
                impl->rasterizer->InvalidateRegion(data.address, data.size);
            } else if constexpr (std::is_same_v<T, AccurateMulCommand>) {
                impl->rasterizer->SetAccurateMul(data.enabled);
            } else if constexpr (std::is_same_v<T, RightEyeCommand>) {
                right_eye_disabler->SetEnabled(data.enabled);
            } else if constexpr (std::is_same_v<T, SwapBuffersCommand>) {
                impl->renderer->SwapBuffers();
            } else if constexpr (std::is_same_v<T, UpdateFramebufferLayoutCommand>) {
                impl->renderer->UpdateCurrentFramebufferLayout(data.is_portrait_mode);
            }
        },
        command.data);
}

void GPU::ProcessSync(const GPUCommandData& data, std::span<const u8> payload) {
    GPUCommand command{};
    command.data = data;
    command.core_id = CurrentCoreId();
    ProcessCommand(command, payload);
}

bool GPU::ShouldUseAsync() const {
    return impl->use_async && !IsOnAsyncGPUThread();
}

// Resolving this once per frame keeps the per-command cost to one load, instead of several
// virtual setting reads plus a movie and debugger query on every guest store to a cached page.
void GPU::RefreshAsyncMode() {
    bool async = impl->async_gpu_enabled && impl->gpu_thread && impl->renderer;
    if (async && (Settings::values.deterministic_async_operations.GetValue() ||
                  Settings::values.use_gdbstub.GetValue() ||
                  impl->system.Movie().GetPlayMode() != Core::Movie::PlayMode::None ||
                  Pica::DebugUtils::IsPicaTracing())) {
        async = false;
    }
    if (async && impl->debug_context) {
        for (const auto& breakpoint : impl->debug_context->breakpoints) {
            if (breakpoint.enabled) {
                async = false;
                break;
            }
        }
    }

    if (impl->use_async && !async) {
        SyncGpuThread();
    }
    impl->use_async = async;
    impl->strict_sync = async && Settings::values.strict_gpu_sync.GetValue();
}

u32 GPU::CurrentCoreId() const {
    if (IsOnAsyncGPUThread()) {
        return impl->gpu_command_core;
    }
    return impl->system.GetRunningCore().GetID();
}

u64 GPU::QueueAsync(const GPUCommandData& command, std::span<const u8> payload) {
    FlushPendingInvalidate();

    const u32 core_id = CurrentCoreId();
    const s64 submit_tick = static_cast<s64>(impl->timing.GetTimer(core_id)->GetTicks());

    auto& gpu_thread = *impl->gpu_thread;
    GPUCommand& slot = gpu_thread.BeginQueue(core_id, submit_tick);
    slot.data = command;
    gpu_thread.WritePayload(slot, payload);
    return gpu_thread.EndQueue();
}

std::span<const u8> GPU::GuestCommandList(PAddr address, u32 size) const {
    const MemoryRef memory = impl->memory.GetPhysicalRef(address);
    const auto source = memory.GetReadBytes<u8>(size);
    if (source.size() != size) {
        LOG_ERROR(HW_GPU, "Cannot read command list at {:#010X} with size {:#X}", address, size);
    }
    return source;
}

void GPU::SignalInterruptFromGPU(Service::GSP::InterruptId interrupt_id, u64 delay_ns,
                                 u32 core_id) {
    constexpr u64 interrupt_bits = 8;
    constexpr u64 max_delay = std::numeric_limits<uintptr_t>::max() >> interrupt_bits;
    ASSERT(delay_ns <= max_delay);
    const uintptr_t event_data =
        static_cast<uintptr_t>((delay_ns << interrupt_bits) | static_cast<u8>(interrupt_id));
    impl->timing.ScheduleEventThreadsafeAt(impl->gpu_command_submit_tick,
                                           impl->async_interrupt_event, event_data, core_id);
}

void GPU::AsyncInterruptCallback(uintptr_t user_data, s64 cycles_late) {
    if (impl->signal_interrupt) {
        constexpr uintptr_t interrupt_mask = 0xFF;
        const auto interrupt_id =
            static_cast<Service::GSP::InterruptId>(user_data & interrupt_mask);
        const u64 delay_ns = static_cast<u64>(user_data >> 8);
        const u64 elapsed_ns = cycles_late > 0 ? cyclesToNs(cycles_late) : 0;
        impl->signal_interrupt(interrupt_id, delay_ns, elapsed_ns);
    }
}

void GPU::QueuePageTableUpdate(PAddr start, u32 size, bool cached) {
    {
        std::scoped_lock lock{impl->page_table_update_mutex};
        impl->page_table_updates.push_back({start, size, cached});
        impl->has_page_table_updates.store(true, std::memory_order_release);
    }
    if (!impl->page_table_update_event_scheduled.exchange(true, std::memory_order_acq_rel)) {
        impl->timing.ScheduleEventThreadsafeAt(impl->gpu_command_submit_tick,
                                               impl->page_table_update_event, 0,
                                               impl->gpu_command_core);
    }
}

void GPU::DrainPageTableUpdates() {
    if (!impl->has_page_table_updates.load(std::memory_order_acquire) || IsOnAsyncGPUThread()) {
        return;
    }

    auto& updates = impl->page_table_update_scratch;
    {
        std::scoped_lock lock{impl->page_table_update_mutex};
        updates.swap(impl->page_table_updates);
        impl->has_page_table_updates.store(false, std::memory_order_release);
    }
    for (const auto& update : updates) {
        impl->memory.RasterizerMarkRegionCached(update.start, update.size, update.cached);
    }
    updates.clear();
}

void GPU::PageTableUpdateCallback(uintptr_t user_data, s64 cycles_late) {
    impl->page_table_update_event_scheduled.store(false, std::memory_order_release);
    DrainPageTableUpdates();
}

bool GPU::IsOnAsyncGPUThread() const {
    return impl->gpu_thread && impl->gpu_thread->IsCurrent();
}

void GPU::RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window) {
    SyncGpuThread();

    // Reset the renderer (this will destroy OpenGL resources)
    impl->renderer.reset();

    // Create a new renderer
    impl->renderer =
        VideoCore::CreateRenderer(emu_window, secondary_window, impl->pica, impl->system);
    impl->rasterizer = impl->renderer->Rasterizer();

    // Rebind the rasterizer to the PICA GPU
    impl->pica.BindRasterizer(impl->rasterizer);

    // Update the sw_blitter with the new rasterizer
    impl->sw_blitter = std::make_unique<SwRenderer::SwBlitter>(impl->memory, impl->rasterizer);

    // Re-apply per-game configuration and reload disk shader cache
    u64 program_id{};
    impl->system.GetAppLoader().ReadProgramId(program_id);
    ApplyPerProgramSettings(program_id);
    if (Settings::values.use_disk_shader_cache) {
        impl->renderer->Rasterizer()->LoadDefaultDiskResources(false, nullptr);
    }

    // Mark ALL GPU registers as dirty so current state gets uploaded to new renderer
    impl->pica.dirty_regs.SetAllDirty();

    // Also mark shader setups as dirty so uniforms get re-uploaded and
    // stale pointers to the old rasterizer's JIT cache are cleared.
    impl->pica.vs_setup.uniforms_dirty = true;
    impl->pica.vs_setup.cached_shader = nullptr;
    impl->pica.gs_setup.uniforms_dirty = true;
    impl->pica.gs_setup.cached_shader = nullptr;

    // Mark all cached LUT/table state in pica as dirty
    impl->pica.lighting.lut_dirty = impl->pica.lighting.LutAllDirty;
    impl->pica.fog.lut_dirty = true;
    impl->pica.proctex.table_dirty = impl->pica.proctex.TableAllDirty;
}

void GPU::ReleaseRenderer() {
    SyncGpuThread();

    // Just reset the renderer to release OpenGL resources
    // Don't null out rasterizer pointer as it will become dangling
    impl->renderer.reset();
    impl->sw_blitter.reset();
    LOG_INFO(HW_GPU, "Renderer released for context destroy");
}

template <class Archive>
void GPU::serialize(Archive& ar, const u32 file_version) {
    SyncGpuThread();
    ar & impl->pica;
    if (Archive::is_loading::value) {
        impl->shadow_regs = impl->pica.regs;
        impl->shadow_regs_lcd = impl->pica.regs_lcd;
        impl->page_table_update_event_scheduled.store(false, std::memory_order_release);
    }
}

SERIALIZE_IMPL(GPU)

} // namespace VideoCore
