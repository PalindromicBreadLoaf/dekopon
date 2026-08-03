// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SwitchFrontend {

struct DecodedImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> rgba;
};

// Decodes a PNG or JPEG off the SD card, downscaled to the smallest size that still covers
// `cover_width` x `cover_height`.
std::string DecodeImageFile(const std::string& path, int cover_width, int cover_height,
                            DecodedImage& out);

} // namespace SwitchFrontend
