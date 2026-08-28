// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdio.h>

static inline FILE* UamGetSystemStderr(void) {
    return stderr;
}

#ifdef stderr
#undef stderr
#endif

#ifdef __cplusplus
extern "C" {
#endif

FILE* UamGetStderr(void);

#ifdef __cplusplus
}
#endif

#define stderr UamGetStderr()
