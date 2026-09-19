// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// The C side of the replacement __libnx_exception_entry.

#include <cstddef>
#include <cstdint>
#include <unistd.h>

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

extern "C" bool DekoponFastmemArenaContains(std::uintptr_t addr);
extern "C" bool DynarmicHorizonHandleFastmemFault(std::uint64_t host_pc, std::uint64_t* new_pc);
extern "C" void _start();

namespace {
// ESR exception classes for a data abort taken from a lower or the current exception level.
constexpr std::uint32_t ESR_EC_DATA_ABORT_LOWER = 0x24;
constexpr std::uint32_t ESR_EC_DATA_ABORT_SAME = 0x25;

constexpr const char* CRASH_PATH = "/switch/dekopon/log/crash.txt";

char s_report[8192];
std::size_t s_report_len;

void Put(char c) {
    if (s_report_len < sizeof(s_report)) {
        s_report[s_report_len++] = c;
    }
}

void Put(const char* text) {
    while (*text != '\0') {
        Put(*text++);
    }
}

void PutHex(std::uint64_t value, int digits) {
    static constexpr char HEX[] = "0123456789abcdef";
    for (int shift = (digits - 1) * 4; shift >= 0; shift -= 4) {
        Put(HEX[(value >> shift) & 0xF]);
    }
}

void PutField(const char* name, std::uint64_t value) {
    Put(name);
    Put(" = ");
    PutHex(value, 16);
    Put("\n");
}

void PutCodeField(const char* name, std::uint64_t value, std::uintptr_t base) {
    Put(name);
    Put(" = ");
    PutHex(value, 16);
    if (value >= base) {
        Put("  (+0x");
        PutHex(value - base, 6);
        Put(")");
    }
    Put("\n");
}

void BuildReport(ThreadExceptionDump* ctx) {
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&_start);

    Put("Dekopon: unhandled CPU exception\n");
    PutField("module base", base);

    if (!threadExceptionIsAArch64(ctx)) {
        PutField("aarch32 pc ", ctx->pc.w);
        return;
    }

    PutField("error_desc ", ctx->error_desc);
    PutField("esr        ", ctx->esr);
    PutCodeField("pc         ", ctx->pc.x, base);
    PutCodeField("lr         ", ctx->lr.x, base);
    PutField("sp         ", ctx->sp.x);
    PutField("fp         ", ctx->fp.x);
    PutField("far        ", ctx->far.x);
    PutField("pstate     ", ctx->pstate);

    for (int i = 0; i < 29; ++i) {
        Put("x");
        Put(i < 10 ? ' ' : static_cast<char>('0' + i / 10));
        Put(static_cast<char>('0' + i % 10));
        Put("         = ");
        PutHex(ctx->cpu_gprs[i].x, 16);
        Put("\n");
    }
}

void WriteReportToSd() {
    FsFileSystem* sdmc = fsdevGetDeviceFileSystem("sdmc");
    if (sdmc == nullptr) {
        return;
    }

    fsFsCreateDirectory(sdmc, "/switch");
    fsFsCreateDirectory(sdmc, "/switch/dekopon");
    fsFsCreateDirectory(sdmc, "/switch/dekopon/log");
    fsFsDeleteFile(sdmc, CRASH_PATH);
    if (R_FAILED(fsFsCreateFile(sdmc, CRASH_PATH, static_cast<s64>(s_report_len), 0))) {
        return;
    }

    FsFile file;
    if (R_FAILED(fsFsOpenFile(sdmc, CRASH_PATH, FsOpenMode_Write, &file))) {
        return;
    }
    fsFileWrite(&file, 0, s_report, s_report_len, FsWriteOption_Flush);
    fsFileClose(&file);
}
}  // namespace

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

extern "C" void __libnx_exception_handler(ThreadExceptionDump* ctx) {
    BuildReport(ctx);
    WriteReportToSd();
    write(STDERR_FILENO, s_report, s_report_len);
}
