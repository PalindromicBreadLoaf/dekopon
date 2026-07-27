// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <initializer_list>

namespace Common::Horizon {

// Pins the calling thread to `core_id`.
bool PinCurrentThread(std::uint32_t core_id);

// Pins the calling thread to the first core in `preferred` that the process is
// allowed to run on.
bool PinCurrentThreadPreferred(std::initializer_list<std::uint32_t> preferred);

// Pins the async GPU thread. Core 2 belongs to the guest JIT and core 1 to audio, so the second
// heavy thread takes the otherwise idle main thread.
bool PinAsyncGpuThread();

// Pins a Vulkan support thread (submit worker, present, fence waiter, shader compiler). These are
// mostly blocked, so they double up with audio rather than with whichever thread is doing GPU work.
bool PinGraphicsSupportThread(bool async_gpu_enabled);

// Returns the core the calling thread is pinned to, or -1 when it is not pinned.
std::int32_t GetCurrentThreadCore();

// Returns the total memory pool available to the process in bytes.
std::uint64_t GetTotalMemorySize();

} // namespace Common::Horizon
