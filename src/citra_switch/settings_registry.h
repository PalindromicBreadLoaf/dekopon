// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace SwitchFrontend {

enum class Category {
    General,
    Graphics,
    Enhancements,
    Stereo3D,
    Audio,
    Layout,
    Controls,
    System,
    Console,
    Storage,
    Advanced,
    Count,
};

inline constexpr int NumCategories = static_cast<int>(Category::Count);

const char* CategoryName(Category category);

enum class QuickSection {
    None,
    Display,
    Graphics,
    Stereo,
    Audio,
    Input,
    System,
    Count,
};

inline constexpr int NumQuickSections = static_cast<int>(QuickSection::Count);

const char* QuickSectionName(QuickSection section);

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
    InstallSecureInfo,
    InstallFriendCodeSeed,
    InstallOtp,
    InstallMovable,
};

namespace EntryFlag {
inline constexpr std::uint32_t None = 0;
// Only takes effect the next time a game is launched.
inline constexpr std::uint32_t Restart = 1u << 0;
// Steps are followed by a framebuffer layout refresh.
inline constexpr std::uint32_t Relayout = 1u << 1;
// Only meaningful while a game is running.
inline constexpr std::uint32_t QuickOnly = 1u << 2;
} // namespace EntryFlag

struct Meta {
    const char* label;
    const char* description;
    std::uint32_t flags = EntryFlag::None;
    QuickSection quick = QuickSection::None;
};

struct SettingEntry {
    const char* id{};
    const char* label{};
    const char* description{};
    const char* group{};
    const char* subgroup{};
    Category category{Category::Count};
    QuickSection quick{QuickSection::None};
    std::uint32_t flags{};

    std::function<std::string()> value;
    std::function<void(int dir)> step;
    std::function<bool()> boolean;
    SettingsModal modal{SettingsModal::None};
    std::function<bool()> quick_visible;

    std::function<std::string()> save;
    std::function<std::string()> default_text;
    std::function<void(std::string_view)> load;

    std::function<bool()> using_global;
    std::function<void(bool)> set_global;
    std::function<std::string()> save_global;

    bool IsPersisted() const {
        return static_cast<bool>(save);
    }
    bool IsOverridable() const {
        return static_cast<bool>(set_global) && IsPersisted();
    }
    bool IsOverridden() const {
        return IsOverridable() && !using_global();
    }
    bool IsShownInQuickMenu() const {
        return !quick_visible || quick_visible();
    }
};

const std::vector<SettingEntry>& Registry();

std::vector<const SettingEntry*> EntriesIn(Category category);

std::vector<const SettingEntry*> EntriesInQuick(QuickSection section);

std::vector<const SettingEntry*> SearchEntries(std::string_view query);

std::vector<const SettingEntry*> OverridableEntriesIn(Category category);

std::vector<const SettingEntry*> SearchOverridableEntries(std::string_view query);

bool CategoryHasOverridables(Category category);

void RestoreGlobalSettings();

const SettingEntry* FindEntry(std::string_view id);

const char* ActiveGraphicsBackendName();

void RefreshShaderCacheSize();

void RefreshUniqueDataStatus();

void ResetEntriesToDefaults();

} // namespace SwitchFrontend
