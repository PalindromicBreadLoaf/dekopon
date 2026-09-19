// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Common::Horizon {

// What the console scans out in each operation mode.
inline constexpr std::uint32_t HandheldWidth = 1280;
inline constexpr std::uint32_t HandheldHeight = 720;
inline constexpr std::uint32_t DockedWidth = 1920;
inline constexpr std::uint32_t DockedHeight = 1080;

bool IsDocked();

bool SetNativeWindowSize(void* native_window, std::uint32_t width, std::uint32_t height);

bool GetNativeWindowSize(void* native_window, std::uint32_t& width, std::uint32_t& height);

} // namespace Common::Horizon
