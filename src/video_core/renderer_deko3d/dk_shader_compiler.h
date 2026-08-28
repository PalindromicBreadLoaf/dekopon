// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string_view>
#include <vector>
#include "common/common_types.h"

namespace Deko3D::ShaderCompiler {

enum class Stage {
    Vertex,
    Geometry,
    Fragment,
    Compute,
};

// Runs complete GLSL through uam and returns the resulting DKSH blob.
[[nodiscard]] std::optional<std::vector<u8>> CompileShader(Stage stage, std::string_view source,
                                                           std::string_view name);

} // namespace Deko3D::ShaderCompiler
