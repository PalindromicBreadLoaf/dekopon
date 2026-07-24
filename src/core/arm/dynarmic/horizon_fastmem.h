// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

// Bridges the guest fastmem arena to the libnx exception dispatcher.
namespace Core::HorizonFastmem {

// Records the host address range the active guest fastmem arena occupies. Passing size 0 clears
// it, which makes every fault fall through to the crash handler again.
void SetActiveArena(std::uintptr_t base, std::size_t size);

bool IsActive();

}  // namespace Core::HorizonFastmem

// True when fastmem is live and addr lands inside the active arena.
extern "C" bool DekoponFastmemArenaContains(std::uintptr_t addr);
