// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// The C side of the replacement __libnx_exception_entry.

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include <switch.h>

#include "horizon_exception_entry.h"

// The stub in horizon_exception_entry.S builds a ThreadExceptionDump from fixed offsets. If libnx
// ever moves a field, these catch it at compile time instead of leaving a dispatcher that silently
// never recognises a fault.
static_assert(offsetof(ThreadExceptionDump, error_desc) == DUMP_ERROR_DESC);
static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == DUMP_GPRS);
static_assert(offsetof(ThreadExceptionDump, fp) == DUMP_FP);
static_assert(offsetof(ThreadExceptionDump, lr) == DUMP_LR);
static_assert(offsetof(ThreadExceptionDump, sp) == DUMP_SP);
static_assert(offsetof(ThreadExceptionDump, pc) == DUMP_PC);
static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == DUMP_FPU);
static_assert(offsetof(ThreadExceptionDump, pstate) == DUMP_PSTATE);
static_assert(offsetof(ThreadExceptionDump, far) == DUMP_FAR);

// core: true when the fault address lands inside the live guest fastmem arena.
extern "C" bool DekoponFastmemArenaContains(std::uintptr_t addr);
// dynarmic: backpatches the fastmem access at host_pc and reports where to resume.
extern "C" bool DynarmicHorizonHandleFastmemFault(std::uint64_t host_pc, std::uint64_t* new_pc);

namespace {
// ESR exception classes for a data abort taken from a lower or the current exception level.
constexpr std::uint32_t ESR_EC_DATA_ABORT_LOWER = 0x24;
constexpr std::uint32_t ESR_EC_DATA_ABORT_SAME = 0x25;
}  // namespace

// Runs in exception context on a private per-fault stack. Returning true resumes the faulting thread
// at ctx->pc, otherwise returning false routes it to the crash handler.
extern "C" bool HorizonExceptionDispatch(ThreadExceptionDump* ctx) {
    if (!threadExceptionIsAArch64(ctx)) {
        return false;
    }

    const std::uint32_t exception_class = (ctx->esr >> 26) & 0x3F;
    if (exception_class != ESR_EC_DATA_ABORT_LOWER && exception_class != ESR_EC_DATA_ABORT_SAME) {
        return false;
    }

    if (!DekoponFastmemArenaContains(static_cast<std::uintptr_t>(ctx->far.x))) {
        return false;
    }

    std::uint64_t new_pc = 0;
    if (!DynarmicHorizonHandleFastmemFault(ctx->pc.x, &new_pc)) {
        return false;
    }

    ctx->pc.x = new_pc;
    return true;
}

// Reached through the crash trampoline in the entry stub when nothing claimed the fault. Expected to
// terminate the process.
extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    std::fprintf(stderr, "Dekopon: unhandled CPU exception\n");

    if (threadExceptionIsAArch64(ctx)) {
        std::fprintf(stderr, "  error_desc=%08" PRIx32 " esr=%08" PRIx32 "\n", ctx->error_desc,
                     ctx->esr);
        std::fprintf(stderr, "  pc =%016" PRIx64 "  lr =%016" PRIx64 "\n", ctx->pc.x, ctx->lr.x);
        std::fprintf(stderr, "  sp =%016" PRIx64 "  far=%016" PRIx64 "\n", ctx->sp.x, ctx->far.x);
        for (int i = 0; i < 29; ++i) {
            std::fprintf(stderr, "  x%-2d=%016" PRIx64 "\n", i, ctx->cpu_gprs[i].x);
        }
    } else {
        std::fprintf(stderr, "  aarch32 exception (pc=%08" PRIx32 ")\n", ctx->pc.w);
    }

    std::fflush(stderr);
}
