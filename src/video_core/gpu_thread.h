// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <thread>
#include <type_traits>
#include <variant>

#include "common/common_types.h"
#include "video_core/pica/regs_external.h"
#include "video_core/pica/regs_lcd.h"

namespace Frontend {
class GraphicsContext;
}

namespace VideoCore {

// Every command payload is trivially copyable. Bulk bytes live in the queue's payload
// arena instead, so publishing a command never allocates.

struct WriteRegCommand {
    VAddr addr{};
    u32 value{};
    u32 mask{~u32{}};
};

/// Payload holds `count` consecutive WriteRegCommands.
struct WriteRegsCommand {
    u32 count{};
};

/// Payload holds the snapshotted command list bytes.
struct SubmitCmdListCommand {
    u32 index{};
    PAddr address{};
};

struct MemoryFillCommand {
    Pica::MemoryFillConfig config{};
    u32 index{};
    u32 interrupt_index{};
};

struct MemoryTransferCommand {
    Pica::DisplayTransferConfig config{};
};

struct BufferSwapCommand {
    u32 screen_id{};
    PAddr address_left{};
    PAddr address_right{};
    u32 active_fb{};
    u32 stride{};
    u32 format{};
    u32 shown_fb{};
};

struct ColorFillCommand {
    Pica::ColorFill fill{};
};

struct AccurateMulCommand {
    bool enabled{};
};

struct RightEyeCommand {
    bool enabled{};
};

struct SwapBuffersCommand {};

struct UpdateFramebufferLayoutCommand {
    bool is_portrait_mode{};
};

struct InvalidateRegionCommand {
    PAddr address{};
    u32 size{};
};

struct StopCommand {};

using GPUCommandData =
    std::variant<StopCommand, WriteRegCommand, WriteRegsCommand, SubmitCmdListCommand,
                 MemoryFillCommand, MemoryTransferCommand, BufferSwapCommand, ColorFillCommand,
                 InvalidateRegionCommand, AccurateMulCommand, RightEyeCommand, SwapBuffersCommand,
                 UpdateFramebufferLayoutCommand>;

static_assert(std::is_trivially_copyable_v<WriteRegCommand>);

struct GPUCommand {
    GPUCommandData data;
    u64 fence{};
    u64 payload_offset{};
    u32 payload_size{};
    u32 core_id{};
    s64 submit_tick{};
};

// The ring is preallocated, so keep an eye on what a slot costs.
static_assert(sizeof(GPUCommand) <= 96);

/**
 * Single-producer/single-consumer command ring with an accompanying byte arena for variable
 * sized payloads. Slots are handed out in place so that neither side ever allocates.
 */
class GPUThread {
public:
    using Handler = std::function<void(const GPUCommand&, std::span<const u8>)>;

    static constexpr std::size_t QueueCapacity = 4096;
    static constexpr std::size_t PayloadCapacity = 4 * 1024 * 1024;

    GPUThread(std::unique_ptr<Frontend::GraphicsContext> context, Handler handler);
    ~GPUThread();

    /// Claims the next ring slot, blocking while the ring is full.
    GPUCommand& BeginQueue(u32 core_id, s64 submit_tick);

    /// Copies `source` into the payload arena and records it on `command`. `source` must fit in
    /// PayloadCapacity.
    /// Callers that cannot guarantee that run the command synchronously instead.
    void WritePayload(GPUCommand& command, std::span<const u8> source);

    /// Publishes the slot claimed by BeginQueue and returns its fence.
    u64 EndQueue();

    void WaitIdle();
    void WaitForFence(u64 target);

    /// True once the consumer has retired everything the producer has published.
    [[nodiscard]] bool IsIdle() const {
        return fence_completed.load(std::memory_order_acquire) >= fence_enqueued;
    }

    [[nodiscard]] u64 LastFence() const {
        return fence_enqueued;
    }

    [[nodiscard]] bool IsCurrent() const {
        return current_thread == this;
    }

private:
    void Run();
    u64 AllocatePayload(u32 size);

    static thread_local const GPUThread* current_thread;

    std::unique_ptr<Frontend::GraphicsContext> context;
    Handler handler;

    // Producer-only state.
    u64 fence_enqueued{};
    std::size_t write_index{};
    std::size_t read_index_cache{};
    u64 payload_head{};
    u64 payload_tail_cache{};

    // Consumer-only state.
    std::size_t read_index{};
    std::size_t write_index_cache{};

    alignas(128) std::atomic_size_t shared_write_index{};
    std::atomic_bool consumer_waiting{};
    alignas(128) std::atomic_size_t shared_read_index{};
    std::atomic_bool producer_waiting{};
    alignas(128) std::atomic<u64> fence_completed{};
    std::atomic_bool fence_waiting{};
    alignas(128) std::atomic<u64> payload_tail{};

    std::array<GPUCommand, QueueCapacity> queue{};
    std::unique_ptr<u8[]> payload_arena;
    std::thread thread;
};

} // namespace VideoCore
