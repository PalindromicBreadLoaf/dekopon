// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Deko3D {

struct DisplayDimensions {
    std::uint32_t width;
    std::uint32_t height;
};

/// Docked yields 1080p, handheld yields 720p.
[[nodiscard]] DisplayDimensions QueryDisplayDimensions();

} // namespace Deko3D
