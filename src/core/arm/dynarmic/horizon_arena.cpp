// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The aliasing technique is ported from the working ARMSX2-NX/Porpoise Switch ports.
// Horizon tracks a state per memory block and only lets a block in the AliasCodeData
// state be aliased into more than one address range, which is exactly what a fastmem arena needs.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <switch.h>

#include "core/arm/dynarmic/horizon_arena.h"

namespace Core::Horizon::Arena {

namespace {

constexpr std::size_t PAGE_SIZE = 0x1000;

struct AliasableSegment {
    void* backing = nullptr;   // opaque heap token
    void* canonical = nullptr; // the AliasCodeData mapping everything goes through
    ::VirtmemReservation* reservation = nullptr;
    std::size_t size = 0;
};

struct ArenaReservation {
    void* base = nullptr;
    ::VirtmemReservation* reservation = nullptr;
    std::size_t size = 0;
};

bool CreateAliasableSegment(AliasableSegment& segment, std::size_t size) {
    const Handle self = envGetOwnProcessHandle();

    void* const backing = std::aligned_alloc(PAGE_SIZE, size);
    if (!backing) {
        std::fprintf(stderr, "arena: aligned_alloc(%zu) failed\n", size);
        return false;
    }

    virtmemLock();
    void* const canonical = virtmemFindCodeMemory(size, PAGE_SIZE);
    ::VirtmemReservation* const reservation =
        canonical ? virtmemAddReservation(canonical, size) : nullptr;
    virtmemUnlock();
    if (!reservation) {
        std::fprintf(stderr, "arena: no %zu bytes of code address space\n", size);
        std::free(backing);
        return false;
    }

    Result result = svcMapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
                                            reinterpret_cast<u64>(backing), size);
    if (R_FAILED(result)) {
        std::fprintf(stderr, "arena: svcMapProcessCodeMemory(%zu) failed: %#010x\n", size, result);
        virtmemLock();
        virtmemRemoveReservation(reservation);
        virtmemUnlock();
        std::free(backing);
        return false;
    }

    // AliasCode -> AliasCodeData. svcSetProcessMemoryPermission is only valid for this one transition.
    result = svcSetProcessMemoryPermission(self, reinterpret_cast<u64>(canonical), size, Perm_Rw);
    if (R_FAILED(result)) {
        std::fprintf(stderr, "arena: svcSetProcessMemoryPermission(%zu) failed: %#010x\n", size,
                     result);
        svcUnmapProcessCodeMemory(self, reinterpret_cast<u64>(canonical),
                                  reinterpret_cast<u64>(backing), size);
        virtmemLock();
        virtmemRemoveReservation(reservation);
        virtmemUnlock();
        std::free(backing);
        return false;
    }

    segment.backing = backing;
    segment.canonical = canonical;
    segment.reservation = reservation;
    segment.size = size;
    return true;
}

// Every alias of these pages must already be gone.
void DestroyAliasableSegment(AliasableSegment& segment) {
    if (!segment.canonical) {
        return;
    }
    const Result result = svcUnmapProcessCodeMemory(
        envGetOwnProcessHandle(), reinterpret_cast<u64>(segment.canonical),
        reinterpret_cast<u64>(segment.backing), segment.size);
    if (R_FAILED(result)) {
        // Leaking address space is survivable.
        std::fprintf(stderr, "arena: svcUnmapProcessCodeMemory failed: %#010x\n", result);
        segment = {};
        return;
    }
    virtmemLock();
    virtmemRemoveReservation(segment.reservation);
    virtmemUnlock();
    std::free(segment.backing);
    segment = {};
}

bool DetectSupport() {
    static constexpr struct {
        unsigned number;
        const char* name;
    } REQUIRED_SYSCALLS[] = {
        {0x02, "svcSetMemoryPermission"},  {0x73, "svcSetProcessMemoryPermission"},
        {0x74, "svcMapProcessMemory"},     {0x75, "svcUnmapProcessMemory"},
        {0x77, "svcMapProcessCodeMemory"}, {0x78, "svcUnmapProcessCodeMemory"},
    };
    for (const auto& syscall : REQUIRED_SYSCALLS) {
        if (!envIsSyscallHinted(syscall.number)) {
            std::fprintf(stderr, "arena: unavailable, %s not hinted.\n",
                         syscall.name);
            return false;
        }
    }
    if (envGetOwnProcessHandle() == INVALID_HANDLE) {
        std::fprintf(stderr, "arena: unavailable, no own-process handle\n");
        return false;
    }

    // Prove one page can actually be aliased and that the two views share memory before trusting any of this.
    AliasableSegment segment;
    if (!CreateAliasableSegment(segment, PAGE_SIZE)) {
        return false;
    }

    virtmemLock();
    void* const alias = virtmemFindAslr(PAGE_SIZE, 0);
    ::VirtmemReservation* const reservation =
        alias ? virtmemAddReservation(alias, PAGE_SIZE) : nullptr;
    virtmemUnlock();
    if (!reservation) {
        std::fprintf(stderr, "arena: unavailable, could not reserve a page to alias into\n");
        DestroyAliasableSegment(segment);
        return false;
    }

    const Handle self = envGetOwnProcessHandle();
    const u64 source = reinterpret_cast<u64>(segment.canonical);
    bool ok = false;
    if (R_SUCCEEDED(svcMapProcessMemory(alias, self, source, PAGE_SIZE))) {
        constexpr u32 PATTERN = 0x12345678;
        *static_cast<volatile u32*>(alias) = PATTERN;
        ok = *reinterpret_cast<volatile u32*>(segment.canonical) == PATTERN;
        if (!ok) {
            std::fprintf(stderr, "arena: unavailable, alias did not share pages\n");
        }
        svcUnmapProcessMemory(alias, self, source, PAGE_SIZE);
    } else {
        std::fprintf(stderr, "arena: unavailable, svcMapProcessMemory failed\n");
    }

    virtmemLock();
    virtmemRemoveReservation(reservation);
    virtmemUnlock();
    DestroyAliasableSegment(segment);
    return ok;
}

}  // namespace

bool IsSupported() {
    static const bool supported = DetectSupport();
    return supported;
}

std::uint8_t* AllocAliasable(std::size_t size, void** out_handle) {
    auto* segment = new AliasableSegment{};
    if (!CreateAliasableSegment(*segment, size)) {
        delete segment;
        *out_handle = nullptr;
        return nullptr;
    }
    *out_handle = segment;
    return static_cast<std::uint8_t*>(segment->canonical);
}

void FreeAliasable(void** handle) {
    if (!*handle) {
        return;
    }
    auto* segment = static_cast<AliasableSegment*>(*handle);
    DestroyAliasableSegment(*segment);
    delete segment;
    *handle = nullptr;
}

std::uint8_t* ReserveArena(std::size_t size, void** out_handle) {
    virtmemLock();
    void* const base = virtmemFindAslr(size, 0);
    ::VirtmemReservation* const reservation = base ? virtmemAddReservation(base, size) : nullptr;
    virtmemUnlock();
    if (!reservation) {
        std::fprintf(stderr, "arena: could not reserve %zu MiB of contiguous space\n",
                     size / 0x100000);
        *out_handle = nullptr;
        return nullptr;
    }
    *out_handle = new ArenaReservation{base, reservation, size};
    return static_cast<std::uint8_t*>(base);
}

void ReleaseArena(void** handle) {
    if (!*handle) {
        return;
    }
    auto* arena = static_cast<ArenaReservation*>(*handle);
    virtmemLock();
    virtmemRemoveReservation(arena->reservation);
    virtmemUnlock();
    delete arena;
    *handle = nullptr;
}

bool MapAlias(std::uintptr_t dest, std::uintptr_t source, std::size_t size) {
    const Result result = svcMapProcessMemory(reinterpret_cast<void*>(dest),
                                              envGetOwnProcessHandle(), source, size);
    if (R_FAILED(result)) {
        std::fprintf(stderr, "arena: svcMapProcessMemory(%p, %#zx) failed: %#010x\n",
                     reinterpret_cast<void*>(dest), size, result);
        return false;
    }
    return true;
}

bool UnmapAlias(std::uintptr_t dest, std::uintptr_t source, std::size_t size) {
    const Result result = svcUnmapProcessMemory(reinterpret_cast<void*>(dest),
                                                envGetOwnProcessHandle(), source, size);
    if (R_FAILED(result)) {
        std::fprintf(stderr, "arena: svcUnmapProcessMemory(%p, %#zx) failed: %#010x\n",
                     reinterpret_cast<void*>(dest), size, result);
        return false;
    }
    return true;
}

}  // namespace Core::Horizon::Arena
