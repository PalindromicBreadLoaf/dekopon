// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/horizon_display.h"

#ifdef __SWITCH__
#include <switch.h>
#endif

namespace Common::Horizon {

#ifdef __SWITCH__

bool IsDocked() {
    return appletGetOperationMode() == AppletOperationMode_Console;
}

bool SetNativeWindowSize(void* native_window, std::uint32_t width, std::uint32_t height) {
    NWindow* window = static_cast<NWindow*>(native_window);
    if (window == nullptr || !nwindowIsValid(window)) {
        return false;
    }
    return R_SUCCEEDED(nwindowSetDimensions(window, width, height));
}

bool GetNativeWindowSize(void* native_window, std::uint32_t& width, std::uint32_t& height) {
    NWindow* window = static_cast<NWindow*>(native_window);
    if (window == nullptr || !nwindowIsValid(window)) {
        return false;
    }
    u32 w = 0;
    u32 h = 0;
    if (R_FAILED(nwindowGetDimensions(window, &w, &h))) {
        return false;
    }
    width = w;
    height = h;
    return true;
}

#else

bool IsDocked() {
    return false;
}

bool SetNativeWindowSize(void*, std::uint32_t, std::uint32_t) {
    return false;
}

bool GetNativeWindowSize(void*, std::uint32_t&, std::uint32_t&) {
    return false;
}

#endif

} // namespace Common::Horizon
