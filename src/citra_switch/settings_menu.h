// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "citra_switch/settings_registry.h"

namespace SwitchFrontend {

struct SettingsRow {
    std::string label;
    std::function<std::string()> value;
    std::function<void(int dir)> step;
    SettingsModal modal{SettingsModal::None};
    std::function<bool()> boolean;
    std::string description;
    bool is_header{};
    bool needs_restart{};
    // Bound only in the per-game editor, for rows the core can hold an override for.
    std::function<bool()> using_global;
    std::function<void(bool)> set_global;
};

std::vector<SettingsRow> BuildCategoryRows(Category category);

std::vector<SettingsRow> BuildQuickRows(QuickSection section);

std::vector<SettingsRow> BuildSearchRows(const std::string& query);

// The same pages restricted to what a single title can override, with the override state bound.
std::vector<SettingsRow> BuildGameCategoryRows(Category category);

std::vector<SettingsRow> BuildGameSearchRows(const std::string& query);

// The log filter string, and applying a new one to the running logger.
std::string GetLogFilter();
void SetLogFilter(const std::string& filter);

// The 3DS profile name
std::string GetProfileUsername();
void SetProfileUsername(const std::string& name);

enum class ProfileValue {
    BirthMonth,
    BirthDay,
    Language,
    SoundMode,
};

int GetProfileValue(ProfileValue field);
void SetProfileValue(ProfileValue field, int value);

int ProfileBirthMonthLength();

bool IsSystemSetupNeeded();
void SetSystemSetupNeeded(bool needed);

int GetPlayCoins();
void SetPlayCoins(int coins);

// One selectable entry in the country picker. `code` is the raw 3DS country code.
struct CountryOption {
    int code;
    const char* name;
};

// Every country the 3DS defines, in code order.
const std::vector<CountryOption>& CountryOptions();
int GetProfileCountry();
void SetProfileCountry(int code);
const char* ProfileCountryName();

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
