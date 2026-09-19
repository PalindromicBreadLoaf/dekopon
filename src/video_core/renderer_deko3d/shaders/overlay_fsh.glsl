// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460

// Overlay fragment shader: modulates a per-batch color by the font atlas
// coverage (single-channel R8). Solid rects sample the atlas' reserved white
// texel, so the same path draws both text and filled panels.

layout (location = 0) in vec2 vTexCoord;
layout (location = 0) out vec4 oColor;

layout (binding = 0) uniform sampler2D uAtlas;

layout (std140, binding = 0) uniform Config {
    vec4 uColor;
};

void main() {
    float coverage = texture(uAtlas, vTexCoord).r;
    oColor = vec4(uColor.rgb, uColor.a * coverage);
}
