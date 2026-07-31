// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// An in-game menu to change quick settings that apply on-the-fly.
namespace SwitchFrontend {

// What the caller should do after a navigation frame.
enum class QuickMenuAction {
    None,
    Close,
    ExitGame,
};

// One frame of navigation.
struct QuickMenuNav {
    bool up{};
    bool down{};
    bool left{};  // Steps a value so the caller feeds it only the d-pad.
    bool right{}; // ^
    bool confirm{};   // A
    bool cancel{};    // B
    bool alt{};       // X
    bool alt2{};      // Y
    bool tab_prev{};  // L
    bool tab_next{};  // R
    bool page_prev{}; // ZL
    bool page_next{}; // ZR
};

// True while the overlay is showing.
bool IsQuickMenuOpen();

// Whether opening the overlay freezes the game.
bool IsPauseInQuickMenu();
void SetPauseInQuickMenu(bool enabled);

// Opens/closes the overlay saving any changed settings.
void OpenQuickMenu();
void CloseQuickMenu();
void ToggleQuickMenu();

// Applies one navigation frame, updates the live settings, and repaints the overlay.
QuickMenuAction UpdateQuickMenu(const QuickMenuNav& nav);

} // namespace SwitchFrontend
