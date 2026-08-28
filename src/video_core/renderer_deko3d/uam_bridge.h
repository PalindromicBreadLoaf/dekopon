// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>

extern "C" {
// Mirrors uam's pipeline_stage enum.
enum UamStage {
    UamStage_Vertex = 0,
    UamStage_TessControl = 1,
    UamStage_TessEval = 2,
    UamStage_Geometry = 3,
    UamStage_Fragment = 4,
    UamStage_Compute = 5,
};

// Compiles GLSL to a DKSH blob.
void* UamCompileGlsl(const char* glsl, int stage, size_t* out_size, char** out_log);
}
