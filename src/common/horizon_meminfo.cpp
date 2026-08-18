// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/horizon_meminfo.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace Common {

#ifdef __SWITCH__

std::uint64_t GetHorizonMemoryUsed() {
    u64 used = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    return used;
}

std::uint64_t GetHorizonMemoryTotal() {
    u64 total = 0;
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    return total;
}

#else

std::uint64_t GetHorizonMemoryUsed() {
    return 0;
}

std::uint64_t GetHorizonMemoryTotal() {
    return 0;
}

#endif

} // namespace Common
