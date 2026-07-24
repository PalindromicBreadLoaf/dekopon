// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Offsets the assembly entry stub needs. The stub cannot see libnx's ThreadExceptionDump, so the
// layout lives here and horizon_exception.cpp static_asserts every field against the real struct.

#define SLOT_COUNT 8
#define SLOT_SHIFT 10
#define SLOT_SIZE (1 << SLOT_SHIFT)
#define SLOT_ALL ((1 << SLOT_COUNT) - 1)
#define STACK_SHIFT 16
#define STACK_SIZE (1 << STACK_SHIFT)

// ThreadExceptionDump, at the start of a slot.
#define DUMP_ERROR_DESC 0x000
#define DUMP_GPRS 0x010
#define DUMP_FP 0x0F8
#define DUMP_LR 0x100
#define DUMP_SP 0x108
#define DUMP_PC 0x110
#define DUMP_PADDING 0x118
#define DUMP_FPU 0x120
#define DUMP_PSTATE 0x320
#define DUMP_FAR 0x330

// Slot bookkeeping, past the end of the dump the handler sees.
#define SLOT_FRAME 0x348
#define SLOT_INDEX 0x350

// ThreadExceptionFrameA64.
#define FRAME_LR 0x48
#define FRAME_SP 0x50
#define FRAME_PC 0x58
#define FRAME_PSTATE 0x60
#define FRAME_FAR 0x70
