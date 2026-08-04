// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

// Only reached if something asks for a passphrase, which nothing does.

#include <openssl/ui.h>

UI_METHOD*
UI_OpenSSL(void) {
    return (UI_METHOD*)UI_null();
}
