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
    Console,
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
    ResetDefaults,
    ClearShaderCache,
    CheckForUpdates,
    ReleaseNotes,
    Username,
    Country,
    FixedClock,
    InitTicksValue,
    ConsoleId,
    MacAddress,
    UnlinkConsole,
    // Ordered to match UniqueDataFile
    InstallSecureInfo,
    InstallFriendCodeSeed,
    InstallOtp,
    InstallMovable,
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

// The 3DS profile name
std::string GetProfileUsername();
void SetProfileUsername(const std::string& name);

// One selectable entry in the country picker. `code` is the raw 3DS country code.
struct CountryOption {
    int code;
    const char* name;
};

// Every country the 3DS defines, in code order.
const std::vector<CountryOption>& CountryOptions();
int GetProfileCountry();
void SetProfileCountry(int code);

// False when the country does not belong to the configured console region.
bool IsCountryValidForRegion(int code);

// The fixed start-up clock, as "YYYY-MM-DD HH:MM:SS".
std::string GetFixedClockText();
bool SetFixedClockText(const std::string& text);

// The fixed initial CPU tick count, as a decimal string.
std::string GetInitTicksText();
void SetInitTicksText(const std::string& text);

std::string GetConsoleIdText();
std::string GetMacAddressText();
void RegenerateConsoleId();
void RegenerateMacAddress();

// The files dumped from a real console that Azahar needs to act as that console.
enum class UniqueDataFile {
    SecureInfo,
    FriendCodeSeed,
    Otp,
    Movable,
    Count,
};

const char* UniqueDataFileName(UniqueDataFile file);

// A short load status.
std::string UniqueDataStatus(UniqueDataFile file);

// Copies `from` over the console's stored copy of `file`.
bool InstallUniqueDataFile(UniqueDataFile file, const std::string& from);

// True once every file needed to impersonate a real console is present, which is what makes
// unlinking meaningful and blocks replacing the files piecemeal.
bool IsConsoleLinked();
void UnlinkConsole();

// Drops the cached read of the CFG savegame so the next visit sees what the last game wrote.
void RefreshSystemSettings();

// Writes pending edits to config.ini, plus the profile data that lives in the CFG NAND savegame
// and the play coin count that lives in the PTM save.
void CommitSettings();

} // namespace SwitchFrontend
