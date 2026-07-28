// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <switch.h>

// The console UI.
namespace SwitchFrontend {

enum class MenuAction {
    Launch, // MenuResult::path holds the ROM to boot.
    Exit,   // Quit the application.
};

struct MenuResult {
    MenuAction action{MenuAction::Exit};
    std::string path;
};

// Draws the library/settings menu on the default nwindow and blocks until the user
// launches a game or exits.
MenuResult RunMenu(PadState& pad);

// Queues a one-shot notice for the next RunMenu entry.
void SetMenuNotice(const std::string& text, bool error = true);

// Frees the font and shared-font resources cached across RunMenu calls.
void ShutdownMenu();

} // namespace SwitchFrontend
