// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

// The rows behind the Settings tab.
namespace SwitchFrontend {

enum class SettingsPage {
    General,
    System,
    Graphics,
    Enhancements,
    Audio,
    Layout,
    Controls,
    Storage,
    Experimental,
    Debug,
    Count,
};

inline constexpr int NumSettingsPages = static_cast<int>(SettingsPage::Count);

// A row that hands off to a modal owned by the menu instead of cycling a value in place.
enum class SettingsModal {
    None,
    LayoutCycle,
    ControllerMap,
    LogFilter,
};

struct SettingsRow {
    const char* label;
    std::function<std::string()> value;
    std::function<void(int dir)> step; // Empty when `modal` is set.
    SettingsModal modal{SettingsModal::None};
    std::function<bool()> boolean; // Set on On/Off rows.
};

const char* SettingsPageName(SettingsPage page);

// The rows of `page`, in display order. Values are read live through the returned callbacks, so
// the list only has to be rebuilt when the page changes.
std::vector<SettingsRow> BuildSettingsPage(SettingsPage page);

// The in-game quick menu's settings-backed pages, in display order. These carry only the subset
// of the pages above that both takes effect without a reboot and is worth reaching for while ingame.
enum class QuickPage {
    Display,
    Graphics,
    Stereo,
    Audio,
    Input,
    System,
    Count,
};

inline constexpr int NumQuickPages = static_cast<int>(QuickPage::Count);

const char* QuickPageName(QuickPage page);

std::vector<SettingsRow> BuildQuickPage(QuickPage page);

// The backend the next launch will actually use, which is not always the configured one.
const char* ActiveGraphicsBackendName();

// The log filter string, and applying a new one to the running logger.
std::string GetLogFilter();
void SetLogFilter(const std::string& filter);

// Writes pending edits to config.ini, plus the 3DS system language to the CFG NAND savegame when
// it changed.
void CommitSettings();

} // namespace SwitchFrontend
