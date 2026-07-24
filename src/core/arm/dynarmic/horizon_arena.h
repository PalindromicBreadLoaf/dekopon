// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

// The libnx primitives the guest fastmem arena is built from.

namespace Core::Horizon::Arena {

// Probes once whether one physical allocation can be aliased into several address ranges on this
// process. Needs the mapping SVCs hinted and a valid own-process handle.
bool IsSupported();

// Allocates `size` bytes in the AliasCodeData state so it can be aliased.
std::uint8_t* AllocAliasable(std::size_t size, void** out_handle);
void FreeAliasable(void** handle);

// Reserves `size` bytes of contiguous host virtual address space for the arena and returns its base,
// or nullptr on failure.
std::uint8_t* ReserveArena(std::size_t size, void** out_handle);
void ReleaseArena(void** handle);

// Aliases [source, source+size) of an aliasable segment at [dest, dest+size), read/write.
bool MapAlias(std::uintptr_t dest, std::uintptr_t source, std::size_t size);
bool UnmapAlias(std::uintptr_t dest, std::uintptr_t source, std::size_t size);

}  // namespace Core::Horizon::Arena
