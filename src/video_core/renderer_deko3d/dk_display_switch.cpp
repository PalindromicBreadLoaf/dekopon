// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <switch.h>

#include "video_core/renderer_deko3d/dk_display_switch.h"

namespace Deko3D {

DisplayDimensions QueryDisplayDimensions() {
    if (appletGetOperationMode() == AppletOperationMode_Console) {
        return {1920, 1080};
    }
    return {1280, 720};
}

} // namespace Deko3D
