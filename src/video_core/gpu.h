// Copyright Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <vector>
#include <boost/serialization/access.hpp>

#include "core/hle/service/gsp/gsp_interrupt.h"
#include "video_core/gpu_thread.h"

namespace Service::GSP {
struct Command;
struct FrameBufferInfo;
} // namespace Service::GSP

namespace Core {
class System;
}

namespace Pica {
class DebugContext;
class PicaCore;
struct RegsLcd;
union ColorFill;
} // namespace Pica

namespace Frontend {
class EmuWindow;
}

namespace VideoCore {

/// Measured on hardware to be 2240568 timer cycles or 4481136 ARM11 cycles
constexpr u64 FRAME_TICKS = 4481136ull;

class GraphicsDebugger;
class RendererBase;
class RightEyeDisabler;

/**
 * The GPU class is the high level interface to the video_core for core services.
 */
class GPU {
public:
    explicit GPU(Core::System& system, Frontend::EmuWindow& emu_window,
                 Frontend::EmuWindow* secondary_window);
    ~GPU();

    /// Sets the function to call for signalling GSP interrupts.
    void SetInterruptHandler(Service::GSP::InterruptHandler handler);

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    void FlushRegion(PAddr addr, u32 size);

    /// Notify rasterizer that any caches of the specified region should be invalidated
    void InvalidateRegion(PAddr addr, u32 size);

    /// Flushes and invalidates caches of the specified region.
    void FlushAndInvalidateRegion(PAddr addr, u32 size);

    /// Flushes and invalidates all memory in the rasterizer cache and removes any leftover state.
    void ClearAll(bool flush);

    /// Waits until all queued GPU work has completed.
    void WaitIdle();

    /// Presents the current renderer output.
    void SwapBuffers();

    /// Recalculates the frontend framebuffer layout on the GPU owner thread.
    void UpdateCurrentFramebufferLayout(bool is_portrait_mode = false);

    /// Executes the provided GSP command.
    void Execute(const Service::GSP::Command& command);

    /// Updates GPU display framebuffer configuration using the specified parameters.
    void SetBufferSwap(u32 screen_id, const Service::GSP::FrameBufferInfo& info);

    /// Sets the LCD color fill configuration for the top and bottom screens.
    void SetColorFill(const Pica::ColorFill& fill);

    /// Reads a word from the GPU virtual address.
    u32 ReadReg(VAddr addr);

    /// Writes the provided value to the GPU virtual address.
    void WriteReg(VAddr addr, u32 data);

    /// Updates selected bits of a GPU register.
    void WriteRegWithMask(VAddr addr, u32 data, u32 mask);

    /// Writes a sequential block of GPU registers.
    void WriteRegs(VAddr addr, std::span<const u8> data, std::span<const u8> masks = {});

    /// Returns a mutable reference to the renderer.
    [[nodiscard]] VideoCore::RendererBase& Renderer();

    /// Returns a mutable reference to the PICA GPU.
    [[nodiscard]] Pica::PicaCore& PicaCore();

    /// Returns an immutable reference to the PICA GPU.
    [[nodiscard]] const Pica::PicaCore& PicaCore() const;

    /// Returns a mutable reference to the pica debugging context.
    [[nodiscard]] Pica::DebugContext& DebugContext();

    /// Returns a mutable reference to the GSP command debugger.
    [[nodiscard]] GraphicsDebugger& Debugger();

    RightEyeDisabler& GetRightEyeDisabler() {
        return *right_eye_disabler;
    }

    void SetRightEyeEnabled(bool enabled);

    void ApplyPerProgramSettings(u64 program_ID);

    /// Recreates the renderer (for GL context reset in libretro)
    void RecreateRenderer(Frontend::EmuWindow& emu_window, Frontend::EmuWindow* secondary_window);

    /// Releases the renderer (for GL context destroy in libretro)
    void ReleaseRenderer();

    void QueuePageTableUpdate(PAddr start, u32 size, bool cached);
    [[nodiscard]] bool IsOnAsyncGPUThread() const;

private:
    /// Describes the side effects a single register write has, as seen by the producer.
    struct RegWriteInfo {
        bool triggers_work{};
        bool has_command_list{};
        u32 command_list_index{};
        PAddr command_list_address{};
        u32 command_list_size{};
    };

    void ProcessCommand(const GPUCommand& command, std::span<const u8> payload);
    void ProcessSync(const GPUCommandData& data, std::span<const u8> payload = {});
    void ProcessWriteReg(const WriteRegCommand& command);
    RegWriteInfo PrepareWriteReg(VAddr addr, u32 data, u32 mask);
    void FlushRegWriteBatch();
    void DispatchCmdList(u32 index, PAddr address, u32 size);
    bool ShouldUseAsync() const;
    void RefreshAsyncMode();
    u32 CurrentCoreId() const;
    u64 QueueAsync(const GPUCommandData& command, std::span<const u8> payload = {});

    /// Quiesces the GPU thread so the caller may touch renderer and PICA state directly.
    void SyncGpuThread();
    void FlushPendingInvalidate();

    void DrainPageTableUpdates();
    void SignalInterruptFromGPU(Service::GSP::InterruptId interrupt_id, u64 delay_ns, u32 core_id);
    [[nodiscard]] std::span<const u8> GuestCommandList(PAddr address, u32 size) const;

    void SubmitCmdList(u32 index);
    void SubmitCmdList(u32 index, PAddr address, std::span<const u8> commands);

    // Interrupt index must be 0 or 1 to signal the relative PSC interrupt.
    void MemoryFill(u32 index, u32 intr_index);

    void MemoryTransfer();

    void VBlankCallback(uintptr_t user_data, s64 cycles_late);
    void AsyncInterruptCallback(uintptr_t user_data, s64 cycles_late);
    void PageTableUpdateCallback(uintptr_t user_data, s64 cycles_late);

    friend class boost::serialization::access;
    template <class Archive>
    void serialize(Archive& ar, const u32 file_version);

    std::unique_ptr<RightEyeDisabler> right_eye_disabler;

private:
    friend class RightEyeDisabler;
    friend class GPUThread;
    struct Impl;
    std::unique_ptr<Impl> impl;

    PAddr VirtualToPhysicalAddress(VAddr addr);
};

} // namespace VideoCore
