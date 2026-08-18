// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>

namespace Common {

// Process-wide memory accounting from the Horizon kernel.
std::uint64_t GetHorizonMemoryUsed();
std::uint64_t GetHorizonMemoryTotal();

} // namespace Common
