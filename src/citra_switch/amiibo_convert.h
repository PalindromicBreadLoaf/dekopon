// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

#include "citra_switch/amiibo_reader.h"

namespace SwitchFrontend {

std::array<std::uint8_t, 0x21C> BuildPlainTag(const AmiiboCapture& capture);

} // namespace SwitchFrontend
