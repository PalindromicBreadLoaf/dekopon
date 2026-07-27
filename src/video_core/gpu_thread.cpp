// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "common/assert.h"
#include "common/horizon_thread.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/frontend/emu_window.h"
#include "video_core/gpu_thread.h"

namespace VideoCore {

namespace {

// A hand-off that is already in flight lands well inside this budget, so spinning first turns the
// common case from two futex syscalls plus two wakeups into a few hundred nanoseconds.
constexpr u32 SpinCount = 1024;

void SpinHint() {
#if defined(__aarch64__) || defined(__arm__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
}

} // namespace

thread_local const GPUThread* GPUThread::current_thread{};

GPUThread::GPUThread(std::unique_ptr<Frontend::GraphicsContext> context_, Handler handler_)
    : context{std::move(context_)}, handler{std::move(handler_)},
      payload_arena{std::make_unique<u8[]>(PayloadCapacity)}, thread{&GPUThread::Run, this} {}

GPUThread::~GPUThread() {
    WaitIdle();
    BeginQueue(0, 0).data = StopCommand{};
    EndQueue();
    thread.join();
}

GPUCommand& GPUThread::BeginQueue(u32 core_id, s64 submit_tick) {
    if (write_index - read_index_cache == QueueCapacity) {
        for (u32 spin = 0; spin < SpinCount; ++spin) {
            read_index_cache = shared_read_index.load(std::memory_order_acquire);
            if (write_index - read_index_cache != QueueCapacity) {
                break;
            }
            SpinHint();
        }
        while (write_index - read_index_cache == QueueCapacity) {
            producer_waiting.store(true, std::memory_order_seq_cst);
            read_index_cache = shared_read_index.load(std::memory_order_seq_cst);
            if (write_index - read_index_cache == QueueCapacity) {
                shared_read_index.wait(read_index_cache, std::memory_order_acquire);
            }
            producer_waiting.store(false, std::memory_order_seq_cst);
            read_index_cache = shared_read_index.load(std::memory_order_acquire);
        }
    }

    GPUCommand& command = queue[write_index % QueueCapacity];
    command.fence = fence_enqueued + 1;
    command.core_id = core_id;
    command.submit_tick = submit_tick;
    command.payload_size = 0;
    return command;
}

void GPUThread::WritePayload(GPUCommand& command, std::span<const u8> source) {
    ASSERT_MSG(source.size() <= PayloadCapacity, "Payload of {:#X} bytes exceeds the arena",
               source.size());

    const u32 size = static_cast<u32>(source.size());
    command.payload_size = size;
    if (size == 0) {
        return;
    }

    const u64 offset = AllocatePayload(size);
    command.payload_offset = offset;
    std::memcpy(payload_arena.get() + (offset % PayloadCapacity), source.data(), size);
}

u64 GPUThread::AllocatePayload(u32 size) {
    // Batched register writes are read back out of the arena as a struct array, so every block
    // starts aligned rather than wherever the previous one happened to end.
    constexpr u64 Alignment = 16;
    u64 offset = (payload_head + Alignment - 1) & ~(Alignment - 1);
    const std::size_t physical = offset % PayloadCapacity;
    if (physical + size > PayloadCapacity) {
        // Skip the arena tail so the block stays contiguous. The consumer frees the gap together
        // with the block that follows it.
        offset += PayloadCapacity - physical;
    }

    const u64 required = offset + size;
    for (u32 spin = 0; required - payload_tail_cache > PayloadCapacity; ++spin) {
        payload_tail_cache = payload_tail.load(std::memory_order_acquire);
        if (required - payload_tail_cache <= PayloadCapacity) {
            break;
        }
        if (spin < SpinCount) {
            SpinHint();
        } else {
            std::this_thread::yield();
        }
    }

    payload_head = required;
    return offset;
}

u64 GPUThread::EndQueue() {
    const u64 fence = ++fence_enqueued;
    shared_write_index.store(++write_index, std::memory_order_seq_cst);
    if (consumer_waiting.load(std::memory_order_seq_cst)) {
        shared_write_index.notify_one();
    }
    return fence;
}

void GPUThread::WaitIdle() {
    WaitForFence(fence_enqueued);
}

void GPUThread::WaitForFence(u64 target) {
    if (IsCurrent() || fence_completed.load(std::memory_order_acquire) >= target) {
        return;
    }

    for (u32 spin = 0; spin < SpinCount; ++spin) {
        if (fence_completed.load(std::memory_order_acquire) >= target) {
            return;
        }
        SpinHint();
    }

    u64 completed = fence_completed.load(std::memory_order_acquire);
    while (completed < target) {
        fence_waiting.store(true, std::memory_order_seq_cst);
        completed = fence_completed.load(std::memory_order_seq_cst);
        if (completed < target) {
            fence_completed.wait(completed, std::memory_order_acquire);
        }
        fence_waiting.store(false, std::memory_order_seq_cst);
        completed = fence_completed.load(std::memory_order_acquire);
    }
}

void GPUThread::Run() {
    current_thread = this;
    Common::SetCurrentThreadName("GPU");
    Common::Horizon::PinAsyncGpuThread();
    LOG_INFO(HW_GPU, "GPU thread running on host core {}", Common::Horizon::GetCurrentThreadCore());
    context->MakeCurrent();

    for (;;) {
        if (read_index == write_index_cache) {
            for (u32 spin = 0; spin < SpinCount; ++spin) {
                write_index_cache = shared_write_index.load(std::memory_order_acquire);
                if (read_index != write_index_cache) {
                    break;
                }
                SpinHint();
            }
            while (read_index == write_index_cache) {
                consumer_waiting.store(true, std::memory_order_seq_cst);
                write_index_cache = shared_write_index.load(std::memory_order_seq_cst);
                if (read_index == write_index_cache) {
                    shared_write_index.wait(write_index_cache, std::memory_order_acquire);
                }
                consumer_waiting.store(false, std::memory_order_seq_cst);
                write_index_cache = shared_write_index.load(std::memory_order_acquire);
            }
        }

        const GPUCommand& command = queue[read_index % QueueCapacity];
        if (std::holds_alternative<StopCommand>(command.data)) {
            break;
        }

        const u64 fence = command.fence;
        const u32 payload_size = command.payload_size;
        const u64 payload_end = command.payload_offset + payload_size;
        handler(command, {payload_arena.get() + (command.payload_offset % PayloadCapacity),
                          payload_size});

        shared_read_index.store(++read_index, std::memory_order_seq_cst);
        if (producer_waiting.load(std::memory_order_seq_cst)) {
            shared_read_index.notify_one();
        }
        if (payload_size != 0) {
            payload_tail.store(payload_end, std::memory_order_release);
        }

        fence_completed.store(fence, std::memory_order_seq_cst);
        if (fence_waiting.load(std::memory_order_seq_cst)) {
            fence_completed.notify_one();
        }
    }

    context->DoneCurrent();
    current_thread = nullptr;
}

} // namespace VideoCore
