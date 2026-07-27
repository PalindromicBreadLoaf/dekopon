// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

#include "common/common_types.h"

// A render-agnostic description of an on-screen overlay menu. The frontend fills it in from the
// input thread and the active renderer reads it back on the emulation thread to draw it on top of
// the game.
namespace VideoCore {

// One line in the overlay. Actions (Resume, Exit, etc.) carry no value.
struct OverlayMenuItem {
    std::string label;
    std::string value;
    bool is_action{};
};

struct OverlayMenuState {
    bool visible{};
    std::string title;
    std::vector<OverlayMenuItem> items;
    int selected{};
    std::string hint; // Footer help text.
};

// Publishes the latest overlay description (called from the input thread).
void SetOverlayMenuState(const OverlayMenuState& state);

// Snapshots the current overlay description (called from the renderer/emulation thread).
OverlayMenuState GetOverlayMenuState();

// Skip the copy above when nothing is shown.
bool IsOverlayMenuVisible();

// Shader-compilation activity.
void NotifyShaderCompileBegin();
void NotifyShaderCompileEnd();
u32 GetPendingShaderCompiles();

// A short-lived message shown over the game, used to report things the player asked for.
// Posting a new one replaces whatever is showing.
void PostOverlayToast(const std::string& text, u32 duration_ms = 2500);

// Returns the live toast text, or an empty string once it has expired.
std::string GetOverlayToast();

} // namespace VideoCore
