// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "core/arm/dynarmic/horizon_fastmem.h"

namespace {
std::atomic<std::uintptr_t> g_arena_base{0};
std::atomic<std::size_t> g_arena_size{0};
}  // namespace

namespace Core::HorizonFastmem {

void SetActiveArena(std::uintptr_t base, std::size_t size) {
    g_arena_base.store(base, std::memory_order_release);
    g_arena_size.store(size, std::memory_order_release);
}

bool IsActive() {
    return g_arena_size.load(std::memory_order_acquire) != 0;
}

}  // namespace Core::HorizonFastmem

extern "C" bool DekoponFastmemArenaContains(std::uintptr_t addr) {
    const std::size_t size = g_arena_size.load(std::memory_order_acquire);
    const std::uintptr_t base = g_arena_base.load(std::memory_order_acquire);
    return size != 0 && addr >= base && addr - base < size;
}
