// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>
#include <type_traits>

#include "audio_core/dsp_interface.h"
#include "citra_switch/config.h"
#include "citra_switch/input.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/overlay_menu.h"
#include "citra_switch/settings_menu.h"
#include "citra_switch/settings_registry.h"
#include "citra_switch/updater.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#ifdef ENABLE_LSFG
#include "video_core/frame_generation.h"
#include "video_core/renderer_vulkan/vk_lsfg.h"
#endif

namespace SwitchFrontend {

namespace {

std::string BoolText(bool on) {
    return on ? "On" : "Off";
}

// Ordered to match Service::CFG::SystemLanguage.
constexpr std::array<const char*, 12> kLanguageNames{
    "Japanese",           "English", "French", "German",     "Italian", "Spanish",
    "Simplified Chinese", "Korean",  "Dutch",  "Portuguese", "Russian", "Traditional Chinese"};

// Ordered to match the SMDH region list.
constexpr std::array<const char*, 8> kRegionNames{"Auto",      "Japan", "USA",   "Europe",
                                                  "Australia", "China", "Korea", "Taiwan"};

// Ordered to match Service::CFG::SoundOutputMode.
constexpr std::array<const char*, 3> kSoundOutputNames{"Mono", "Stereo", "Surround"};

constexpr std::array<const char*, 12> kMonthNames{"January",   "February", "March",    "April",
                                                  "May",       "June",     "July",     "August",
                                                  "September", "October",  "November", "December"};

constexpr std::array<const char*, 2> kInitClockNames{"System time", "Fixed time"};

// Ordered to match Settings::InitTicks.
constexpr std::array<const char*, 2> kInitTicksNames{"Random", "Fixed"};

constexpr std::array<const char*, 6> kTextureFilterNames{"None",       "Anime4K", "Bicubic",
                                                         "ScaleForce", "xBRZ",    "MMPX"};

constexpr std::array<const char*, 3> kTextureSamplingNames{"Game controlled", "Nearest neighbour",
                                                           "Linear"};

constexpr std::array<const char*, 5> kAnisotropyNames{"Off", "2x", "4x", "8x", "16x"};

constexpr std::array<const char*, 3> kAudioEmulationNames{"HLE", "LLE", "LLE (multithreaded)"};

constexpr std::array<const char*, 7> kStereoNames{
    "Off",        "Side by side",       "Side by side (full)", "Anaglyph",
    "Interlaced", "Reverse interlaced", "Nintendo Labo VR"};

constexpr std::array<const char*, 2> kMonoEyeNames{"Left eye", "Right eye"};

// Matches Settings::SmallScreenPosition.
constexpr std::array<const char*, 8> kOverlayPositionNames{
    "Top right",   "Middle right", "Bottom right", "Top left",
    "Middle left", "Bottom left",  "Top centre",   "Bottom centre"};

constexpr std::array<const char*, 2> kUpdateChannelNames{"Stable", "Prerelease"};

constexpr std::array kGraphicsApis{
#ifdef ENABLE_SOFTWARE_RENDERER
    Settings::GraphicsAPI::Software,
#endif
#ifdef ENABLE_OPENGL
    Settings::GraphicsAPI::OpenGL,
#endif
#ifdef ENABLE_VULKAN
    Settings::GraphicsAPI::Vulkan,
#endif
};

const char* GraphicsApiName(Settings::GraphicsAPI api) {
    switch (api) {
    case Settings::GraphicsAPI::Software:
        return "Software";
    case Settings::GraphicsAPI::OpenGL:
        return "OpenGL";
    case Settings::GraphicsAPI::Vulkan:
        return "Vulkan";
    default:
        return "Unknown";
    }
}

std::uint64_t s_shader_cache_size = 0;
std::array<std::string, static_cast<std::size_t>(UniqueDataFile::Count)> s_unique_data_status;

// config.ini text conversions.

std::string ToText(bool value) {
    return value ? "true" : "false";
}

std::string ToText(const std::string& value) {
    return value;
}

template <typename T>
std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>, std::string> ToText(T value) {
    std::ostringstream ss;
    if constexpr (std::is_enum_v<T>) {
        ss << static_cast<long long>(value);
    } else {
        ss << value;
    }
    return ss.str();
}

bool FromText(std::string_view text, bool fallback) {
    std::string lower;
    lower.reserve(text.size());
    for (const char c : text) {
        lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    if (lower == "true" || lower == "yes" || lower == "on" || lower == "1") {
        return true;
    }
    if (lower == "false" || lower == "no" || lower == "off" || lower == "0") {
        return false;
    }
    return fallback;
}

std::string FromText(std::string_view text, const std::string& fallback) {
    return text.empty() ? fallback : std::string{text};
}

template <typename T>
std::enable_if_t<std::is_arithmetic_v<T> || std::is_enum_v<T>, T> FromText(std::string_view text,
                                                                          T fallback) {
    std::istringstream ss{std::string{text}};
    if constexpr (std::is_enum_v<T>) {
        long long raw = 0;
        return ss >> raw ? static_cast<T>(raw) : fallback;
    } else {
        T value{};
        return ss >> value ? value : fallback;
    }
}

template <typename S>
using ValueOf = std::decay_t<decltype(std::declval<S&>().GetValue())>;

// Binds config.ini round-tripping to a Settings::values member, taking the key from its label.
template <typename S>
void Persist(SettingEntry& entry, S& setting) {
    using T = ValueOf<S>;
    entry.id = setting.GetLabel().c_str();
    entry.save = [&setting] { return ToText(setting.GetValue()); };
    entry.default_text = [&setting] { return ToText(setting.GetDefault()); };
    entry.load = [&setting](std::string_view text) {
        setting = FromText(text, static_cast<T>(setting.GetValue()));
    };
    if constexpr (requires { setting.UsingGlobal(); }) {
        entry.using_global = [&setting] { return setting.UsingGlobal(); };
        entry.save_global = [&setting] { return ToText(setting.GetValue(true)); };
        entry.set_global = [&setting](bool global) {
            if (!global && setting.UsingGlobal()) {
                const T seed = static_cast<T>(setting.GetValue(true));
                setting.SetGlobal(false);
                setting = seed;
                return;
            }
            setting.SetGlobal(global);
        };
    }
}

// Same, for a value the frontend keeps outside Settings::values.
template <typename T, typename Get, typename Set>
void PersistLocal(SettingEntry& entry, const char* id, Get get, Set set, T fallback) {
    entry.id = id;
    entry.save = [get] { return ToText(static_cast<T>(get())); };
    entry.default_text = [fallback] { return ToText(fallback); };
    entry.load = [get, set](std::string_view text) { set(FromText(text, static_cast<T>(get()))); };
}

SettingEntry Base(const Meta& meta) {
    SettingEntry entry;
    entry.label = meta.label;
    entry.description = meta.description;
    entry.flags = meta.flags;
    entry.quick = meta.quick;
    return entry;
}

template <typename S>
SettingEntry Toggle(const Meta& meta, S& setting) {
    SettingEntry entry = Base(meta);
    const auto get = [&setting] { return static_cast<bool>(setting.GetValue()); };
    entry.value = [get] { return BoolText(get()); };
    entry.step = [&setting](int dir) { setting = dir > 0; };
    entry.boolean = get;
    Persist(entry, setting);
    return entry;
}

template <typename S>
SettingEntry Number(const Meta& meta, S& setting, int lo, int hi, int step,
                    const char* suffix = "") {
    SettingEntry entry = Base(meta);
    entry.value = [&setting, suffix] {
        return std::to_string(static_cast<int>(setting.GetValue())) + suffix;
    };
    entry.step = [&setting, lo, hi, step](int dir) {
        setting = static_cast<ValueOf<S>>(
            std::clamp(static_cast<int>(setting.GetValue()) + dir * step, lo, hi));
    };
    Persist(entry, setting);
    return entry;
}

template <typename S, std::size_t N>
SettingEntry Choice(const Meta& meta, S& setting, const std::array<const char*, N>& names) {
    SettingEntry entry = Base(meta);
    entry.value = [&setting, &names] {
        const auto index = static_cast<std::size_t>(setting.GetValue());
        return std::string{index < names.size() ? names[index] : "Unknown"};
    };
    entry.step = [&setting, &names](int dir) {
        setting = static_cast<ValueOf<S>>(
            std::clamp(static_cast<int>(setting.GetValue()) + dir, 0, static_cast<int>(N) - 1));
    };
    Persist(entry, setting);
    return entry;
}

template <typename S>
SettingEntry Percent(const Meta& meta, S& setting, int lo, int hi, int step) {
    SettingEntry entry = Base(meta);
    const auto as_percent = [](float value) {
        return static_cast<int>(std::lround(value * 100.0f));
    };
    entry.value = [&setting, as_percent] {
        return std::to_string(as_percent(setting.GetValue())) + "%";
    };
    entry.step = [&setting, as_percent, lo, hi, step](int dir) {
        setting =
            static_cast<float>(std::clamp(as_percent(setting.GetValue()) + dir * step, lo, hi)) /
            100.0f;
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry ColorChannel(const Meta& meta, Settings::SwitchableSetting<float>& setting) {
    SettingEntry entry = Base(meta);
    const auto as_byte = [](float value) { return static_cast<int>(std::lround(value * 255.0f)); };
    entry.value = [&setting, as_byte] { return std::to_string(as_byte(setting.GetValue())); };
    entry.step = [&setting, as_byte](int dir) {
        setting = static_cast<float>(std::clamp(as_byte(setting.GetValue()) + dir * 8, 0, 255)) /
                  255.0f;
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry Modal(const Meta& meta, SettingsModal modal, std::function<std::string()> value) {
    SettingEntry entry = Base(meta);
    entry.id = meta.label;
    entry.modal = modal;
    entry.value = std::move(value);
    return entry;
}

template <typename S>
SettingEntry PersistedModal(const Meta& meta, SettingsModal modal, S& setting,
                            std::function<std::string()> value) {
    SettingEntry entry = Modal(meta, modal, std::move(value));
    Persist(entry, setting);
    return entry;
}

SettingEntry LocalBool(const char* id, const Meta& meta, bool (*get)(), void (*set)(bool),
                       bool fallback) {
    SettingEntry entry = Base(meta);
    entry.value = [get] { return BoolText(get()); };
    entry.step = [set](int dir) { set(dir > 0); };
    entry.boolean = [get] { return get(); };
    PersistLocal<bool>(entry, id, get, set, fallback);
    return entry;
}

SettingEntry LocalNumber(const char* id, const Meta& meta, std::function<int()> get,
                         std::function<void(int)> set, int fallback, int lo, int hi, int step,
                         const char* suffix = "") {
    SettingEntry entry = Base(meta);
    entry.value = [get, suffix] { return std::to_string(get()) + suffix; };
    entry.step = [get, set, lo, hi, step](int dir) {
        set(std::clamp(get() + dir * step, lo, hi));
    };
    PersistLocal<int>(entry, id, get, set, fallback);
    return entry;
}

template <std::size_t N>
SettingEntry LocalChoice(const char* id, const Meta& meta, std::function<int()> get,
                         std::function<void(int)> set, int fallback,
                         const std::array<const char*, N>& names) {
    SettingEntry entry = Base(meta);
    entry.value = [get, &names] {
        const auto index = static_cast<std::size_t>(get());
        return std::string{index < names.size() ? names[index] : "Unknown"};
    };
    entry.step = [get, set](int dir) {
        set(std::clamp(get() + dir, 0, static_cast<int>(N) - 1));
    };
    PersistLocal<int>(entry, id, get, set, fallback);
    return entry;
}

template <std::size_t N>
SettingEntry ProfileChoice(const Meta& meta, ProfileValue field,
                           const std::array<const char*, N>& names, int lo = 0) {
    SettingEntry entry = Base(meta);
    entry.id = meta.label;
    entry.value = [field, &names, lo] {
        const auto index = static_cast<std::size_t>(GetProfileValue(field) - lo);
        return std::string{index < names.size() ? names[index] : "Unknown"};
    };
    entry.step = [field, lo](int dir) {
        SetProfileValue(field, std::clamp(GetProfileValue(field) + dir, lo,
                                          lo + static_cast<int>(N) - 1));
    };
    return entry;
}

class Table {
public:
    Table(std::vector<SettingEntry>& out, Category category) : out{out}, category{category} {}

    void Group(const char* name) {
        subgroup = name;
    }

    void Ini(const char* name) {
        section = name;
    }

    Table& operator<<(SettingEntry entry) {
        if ((entry.flags & EntryFlag::QuickOnly) == 0) {
            entry.category = category;
        }
        entry.subgroup = subgroup;
        if (entry.IsPersisted()) {
            entry.group = section;
        }
        out.push_back(std::move(entry));
        return *this;
    }

private:
    std::vector<SettingEntry>& out;
    Category category;
    const char* subgroup = "";
    const char* section = "Miscellaneous";
};

SettingEntry Relayout(SettingEntry entry) {
    entry.flags |= EntryFlag::Relayout;
    entry.step = [step = std::move(entry.step)](int dir) {
        step(dir);
        RequestLayoutUpdate();
    };
    return entry;
}

bool StereoIsOff() {
    return Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::Off;
}

SettingEntry WithDefault(SettingEntry entry, const char* text) {
    entry.default_text = [text] { return std::string{text}; };
    return entry;
}

SettingEntry LocalEnum(const char* id, const Meta& meta, std::function<int()> get,
                       std::function<void(int)> set, int fallback, int count,
                       std::function<const char*(int)> name) {
    SettingEntry entry = Base(meta);
    entry.value = [get, name] { return std::string{name(get())}; };
    entry.step = [get, set, count](int dir) { set(std::clamp(get() + dir, 0, count - 1)); };
    PersistLocal<int>(entry, id, get, set, fallback);
    return entry;
}

SettingEntry GraphicsApiEntry() {
    auto& setting = Settings::values.graphics_api;
    SettingEntry entry = Base({"Graphics API",
                               "Renderer the next launch uses.",
                               EntryFlag::Restart});
    entry.value = [&setting] { return std::string{GraphicsApiName(setting.GetValue())}; };
    entry.step = [&setting](int dir) {
        const auto it = std::find(kGraphicsApis.begin(), kGraphicsApis.end(), setting.GetValue());
        const int current =
            it == kGraphicsApis.end() ? 0 : static_cast<int>(it - kGraphicsApis.begin());
        setting = kGraphicsApis[std::clamp(current + dir, 0,
                                           static_cast<int>(kGraphicsApis.size()) - 1)];
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry FrameLimitEntry() {
    auto& setting = Settings::values.frame_limit;
    SettingEntry entry =
        Base({"Frame Limit", "Speed cap as a percentage of full speed. Off removes any cap.",
              EntryFlag::None, QuickSection::System});
    entry.value = [&setting] {
        const int limit = static_cast<int>(setting.GetValue());
        return limit == 0 ? std::string{"Off"} : std::to_string(limit) + "%";
    };
    entry.step = [&setting](int dir) {
        setting =
            static_cast<double>(std::clamp(static_cast<int>(setting.GetValue()) + dir * 5, 0, 1000));
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry ResolutionEntry() {
    auto& setting = Settings::values.resolution_factor;
    SettingEntry entry = Base({"Internal Resolution",
                               "Render scale. Above native costs GPU time but sharpens 3D.",
                               EntryFlag::None, QuickSection::Graphics});
    entry.value = [&setting] {
        switch (setting.GetValue()) {
        case 0:
            return std::string{"Auto (window)"};
        case 1:
            return std::string{"Native (1x)"};
        default:
            return std::to_string(setting.GetValue()) + "x";
        }
    };
    entry.step = [&setting](int dir) {
        setting = static_cast<u32>(std::clamp(static_cast<int>(setting.GetValue()) + dir, 0, 4));
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry RegionEntry() {
    auto& setting = Settings::values.region_value;
    SettingEntry entry = Base({"Console Region",
                               "Region the emulated console reports. Auto follows the game."});
    entry.value = [&setting] {
        const auto index = static_cast<std::size_t>(setting.GetValue() + 1);
        return std::string{index < kRegionNames.size() ? kRegionNames[index] : "Auto"};
    };
    entry.step = [&setting](int dir) {
        setting = static_cast<s32>(std::clamp(setting.GetValue() + dir, -1, 6));
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry StereoModeEntry() {
    SettingEntry entry = Choice({"Stereoscopic 3D", "How the two eyes are presented.",
                                 EntryFlag::None, QuickSection::Stereo},
                                Settings::values.render_3d, kStereoNames);
    entry.step = [step = std::move(entry.step)](int dir) {
        step(dir);
        if (!StereoIsOff()) {
            if (Settings::values.factor_3d.GetValue() == 0) {
                Settings::values.factor_3d = 60;
            }
            Settings::values.disable_right_eye_render = false;
        }
    };
    return Relayout(std::move(entry));
}

SettingEntry StereoDepthEntry() {
    SettingEntry entry =
        Number({"Stereoscopic Depth", "How far apart the two eyes are drawn.", EntryFlag::None,
                QuickSection::Stereo},
               Settings::values.factor_3d, 0, 100, 5, "%");
    entry.value = [value = std::move(entry.value)] {
        return StereoIsOff() ? std::string{"Needs 3D"} : value();
    };
    entry.step = [step = std::move(entry.step)](int dir) {
        if (!StereoIsOff()) {
            step(dir);
        }
    };
    entry.default_text = [] { return std::string{StereoIsOff() ? "0" : "60"}; };
    return entry;
}

SettingEntry DisableRightEyeEntry() {
    auto& setting = Settings::values.disable_right_eye_render;
    SettingEntry entry = Base({"Disable Right Eye Render",
                               "Skips the top screen's second eye. Much faster in some games, "
                               "flickers/bugs in others.",
                               EntryFlag::None, QuickSection::Stereo});
    entry.value = [&setting] {
        return StereoIsOff() ? BoolText(setting.GetValue()) : std::string{"Required for 3D"};
    };
    entry.step = [&setting](int dir) {
        if (StereoIsOff()) {
            setting = dir > 0;
        }
    };
    entry.boolean = [&setting] { return StereoIsOff() && setting.GetValue(); };
    Persist(entry, setting);
    return entry;
}

SettingEntry LargeScreenProportionEntry() {
    auto& setting = Settings::values.large_screen_proportion;
    SettingEntry entry =
        Base({"Large Screen Proportion", "How much bigger the large screen is than the small one.",
              EntryFlag::None, QuickSection::Display});
    entry.value = [&setting] {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%.2fx", setting.GetValue());
        return std::string{buffer};
    };
    entry.step = [&setting](int dir) {
        setting = std::clamp(setting.GetValue() + dir * 0.25f, 1.0f, 16.0f);
    };
    Persist(entry, setting);
    return Relayout(std::move(entry));
}

SettingEntry CpuClockEntry() {
    auto& setting = Settings::values.cpu_clock_percentage;
    SettingEntry entry = Base({"CPU Clock",
                               "Emulated ARM11 speed. Under 100% can help slow games, over it "
                               "breaks timing-sensitive ones.",
                               EntryFlag::None, QuickSection::System});
    entry.value = [&setting] { return std::to_string(setting.GetValue()) + "%"; };
    entry.step = [&setting](int dir) {
        setting = std::clamp(setting.GetValue() + dir * 5, 25, 400);
        auto& system = Core::System::GetInstance();
        if (system.IsPoweredOn()) {
            system.CoreTiming().UpdateClockSpeed(static_cast<u32>(setting.GetValue()));
        }
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry AudioStretchingEntry() {
    auto& setting = Settings::values.enable_audio_stretching;
    SettingEntry entry = Base({"Audio Stretching",
                               "Resamples audio to hide dropouts when the emulator runs slow.",
                               EntryFlag::None, QuickSection::Audio});
    const auto get = [&setting] { return setting.GetValue(); };
    entry.value = [get] { return BoolText(get()); };
    entry.step = [&setting](int dir) {
        setting = dir > 0;
        auto& system = Core::System::GetInstance();
        if (system.IsPoweredOn()) {
            system.DSP().EnableStretching(dir > 0);
        }
    };
    entry.boolean = get;
    Persist(entry, setting);
    return entry;
}

SettingEntry ClockOffsetEntry() {
    auto& setting = Settings::values.init_time_offset;
    SettingEntry entry =
        Base({"Clock Offset", "Days added to the emulated clock, for games gated on the date."});
    entry.value = [&setting] {
        const long long days = setting.GetValue() / 86400;
        return std::to_string(days) + (days == 1 || days == -1 ? " day" : " days");
    };
    entry.step = [&setting](int dir) {
        const long long days =
            std::clamp<long long>(setting.GetValue() / 86400 + dir, -3650, 3650);
        setting = static_cast<s64>(days * 86400);
    };
    Persist(entry, setting);
    return entry;
}

SettingEntry PlayCoinsEntry() {
    SettingEntry entry = Base({"Play Coins", "Coins the emulated console holds, max 300."});
    entry.id = "Play Coins";
    entry.value = [] { return std::to_string(GetPlayCoins()); };
    entry.step = [](int dir) { SetPlayCoins(std::clamp(GetPlayCoins() + dir, 0, 300)); };
    return entry;
}

SettingEntry BirthMonthEntry() {
    SettingEntry entry = ProfileChoice({"Birthday Month", "Month the console profile was born in."},
                                       ProfileValue::BirthMonth, kMonthNames, 1);
    entry.step = [step = std::move(entry.step)](int dir) {
        step(dir);
        SetProfileValue(ProfileValue::BirthDay,
                        std::min(GetProfileValue(ProfileValue::BirthDay),
                                 ProfileBirthMonthLength()));
    };
    return entry;
}

SettingEntry BirthDayEntry() {
    SettingEntry entry = Base({"Birthday Day", "Day of the month the console profile was born on."});
    entry.id = "Birthday Day";
    entry.value = [] { return std::to_string(GetProfileValue(ProfileValue::BirthDay)); };
    entry.step = [](int dir) {
        SetProfileValue(ProfileValue::BirthDay,
                        std::clamp(GetProfileValue(ProfileValue::BirthDay) + dir, 1,
                                   ProfileBirthMonthLength()));
    };
    return entry;
}

SettingEntry SystemSetupEntry() {
    SettingEntry entry = Base({"System Setup Required",
                               "Whether the emulated console still wants its first-boot setup."});
    entry.id = "System Setup Required";
    entry.value = [] { return BoolText(IsSystemSetupNeeded()); };
    entry.step = [](int dir) { SetSystemSetupNeeded(dir > 0); };
    entry.boolean = [] { return IsSystemSetupNeeded(); };
    return entry;
}

SettingEntry UniqueDataEntry(UniqueDataFile file, const char* description) {
    const auto modal = static_cast<SettingsModal>(static_cast<int>(SettingsModal::InstallSecureInfo) +
                                                  static_cast<int>(file));
    const auto slot = static_cast<std::size_t>(file);
    return Modal({UniqueDataFileName(file), description}, modal,
                 [slot] { return s_unique_data_status[slot]; });
}

SettingEntry LayoutCycleEntry() {
    SettingEntry entry = Modal({"R3 Screen Layouts",
                                "Which screen arrangements the R3 button cycles between in game."},
                               SettingsModal::LayoutCycle, [] {
                                   const int total = GetScreenLayoutCount();
                                   const std::uint32_t mask = GetLayoutCycleMask();
                                   int enabled = 0;
                                   for (int i = 0; i < total; ++i) {
                                       enabled += (mask & (1u << i)) != 0 ? 1 : 0;
                                   }
                                   return std::to_string(enabled) + " of " + std::to_string(total);
                               });
    PersistLocal<int>(
        entry, "layout_cycle_mask", [] { return static_cast<int>(GetLayoutCycleMask()); },
        [](int mask) { SetLayoutCycleMask(static_cast<std::uint32_t>(mask)); },
        (1 << GetScreenLayoutCount()) - 1);
    return entry;
}

SettingEntry ClearShaderCacheEntry() {
    return Modal({"Clear Shader Cache",
                  "Deletes every game's compiled shaders. Expect stutters on the next run."},
                 SettingsModal::ClearShaderCache,
                 [] { return FormatSize(s_shader_cache_size); });
}

#ifdef ENABLE_LSFG
SettingEntry FrameGenerationEntry() {
    auto& setting = Settings::values.use_frame_generation;
    SettingEntry entry = Base({"Frame Generation",
                               "Interpolates extra frames between rendered ones to smooth motion.",
                               EntryFlag::None, QuickSection::Graphics});
    const auto get = [&setting] { return setting.GetValue(); };
    entry.value = [get] {
        if (!get()) {
            return BoolText(false);
        }
        const auto decision = VideoCore::GetFrameGenerationDecision();
        std::string text = "On: " + std::string{VideoCore::FrameGenerationStateName(decision.state)};
        if (decision.multiplier != 0) {
            text += " " + std::to_string(decision.multiplier) + "x";
        }
        return text;
    };
    entry.step = [&setting](int dir) { setting = dir > 0; };
    entry.boolean = get;
    Persist(entry, setting);
    return entry;
}

SettingEntry LosslessDllEntry() {
    const bool present = Vulkan::IsLsfgShaderDllPresent();
    SettingEntry entry =
        Base({"Lossless.dll", "Frame generation needs this file under lsfg/ to run."});
    entry.id = "Lossless.dll";
    entry.value = [present] { return std::string{present ? "Found" : "Missing"}; };
    entry.step = [](int) {};
    return entry;
}
#endif

SettingEntry ScreenLayoutEntry() {
    SettingEntry entry = Base({"Screen Layout", "How the two 3DS screens are arranged.",
                               EntryFlag::QuickOnly, QuickSection::Display});
    entry.id = "Screen Layout";
    entry.value = [] { return std::string{CurrentScreenLayoutName()}; };
    entry.step = [](int dir) { StepScreenLayout(dir); };
    return entry;
}

SettingEntry SwapScreensEntry() {
    SettingEntry entry = Base({"Swap Screens", "Puts the bottom screen where the top one was.",
                               EntryFlag::QuickOnly, QuickSection::Display});
    entry.id = "Swap Screens";
    entry.value = [] { return std::string{}; };
    entry.step = [](int) { ToggleSwapScreens(); };
    return entry;
}

SettingEntry PointerModeEntry() {
    SettingEntry entry = Base({"Pointer Mode", "Drives the 3DS touch screen with a stick or gyro.",
                               EntryFlag::QuickOnly, QuickSection::Input});
    entry.id = "Pointer Mode";
    entry.value = [] { return BoolText(IsPointerModeActive()); };
    entry.step = [](int dir) { SetPointerMode(dir > 0); };
    entry.boolean = [] { return IsPointerModeActive(); };
    return entry;
}

SettingEntry GyroSensitivityEntry(bool horizontal) {
    const char* label = horizontal ? "Gyro Sensitivity X" : "Gyro Sensitivity Y";
    const char* id = horizontal ? "gyro_sensitivity_x" : "gyro_sensitivity_y";
    const char* description = horizontal ? "How fast the gyro pointer moves left and right."
                                         : "How fast the gyro pointer moves up and down.";
    return LocalNumber(
        id, {label, description, EntryFlag::None, QuickSection::Input},
        [horizontal] { return horizontal ? GetGyroSensitivityX() : GetGyroSensitivityY(); },
        [horizontal](int value) {
            SetGyroSensitivity(horizontal ? value : GetGyroSensitivityX(),
                               horizontal ? GetGyroSensitivityY() : value);
        },
        100, 10, 500, 10, "%");
}

void BuildGeneral(std::vector<SettingEntry>& out) {
    Table t{out, Category::General};

    t.Group("Performance");
    t.Ini("Renderer");
    t << FrameLimitEntry();
    t.Ini("Core");
    t << CpuClockEntry();

    t.Group("On-Screen");
    t.Ini("Renderer");
    t << Toggle({"Show FPS Counter", "Draws a framerate counter over the game.", EntryFlag::None,
                 QuickSection::Display},
                Settings::values.show_fps);
    t << Toggle({"Shader Compile Notice",
                 "Note on screen while shaders are being compiled (uses some performance).", EntryFlag::None,
                 QuickSection::Display},
                Settings::values.show_shader_compile_notice);

    t.Group("Quick Menu");
    t.Ini("Switch");
    t << LocalBool("pause_in_quick_menu",
                   {"Pause In Quick Menu", "Freezes the game while the in-game menu is open.",
                    EntryFlag::None, QuickSection::System},
                   IsPauseInQuickMenu, SetPauseInQuickMenu, false);

    t.Group("Updates");
    t << LocalChoice(
        "update_channel",
        {"Update Channel", "Which GitHub releases the updater offers you."},
        [] { return static_cast<int>(GetUpdateChannel()); },
        [](int value) { SetUpdateChannel(static_cast<UpdateChannel>(value)); }, 0,
        kUpdateChannelNames);
    t << LocalBool("show_whats_new",
                   {"Show What's New", "Shows the release notes card after an update."},
                   IsWhatsNewCardEnabled, SetWhatsNewCardEnabled, true);
    t << Modal({"Check for Updates", "Asks GitHub whether a newer build is available."},
               SettingsModal::CheckForUpdates, [] { return std::string{CurrentVersion()}; });
    t << Modal({"Release Notes", "Shows what changed in the build you are running."},
               SettingsModal::ReleaseNotes, [] { return std::string{CurrentVersion()}; });

    t.Group("Maintenance");
    t << Modal({"Reset All Settings",
                "Returns every setting to a preset. Folders, titles and saves are untouched."},
               SettingsModal::ResetDefaults, [] { return std::string{"Choose"}; });
}

void BuildGraphics(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::Graphics};
    t.Ini("Renderer");

    t.Group("Backend");
    t << GraphicsApiEntry();
    t << Toggle({"SPIR-V Shader Generation",
                 "Emits SPIR-V directly instead of going through GLSL.", EntryFlag::Restart},
                v.spirv_shader_gen);
    t << Toggle({"Disable SPIR-V Optimizer",
                 "Skips the SPIR-V optimiser. Compiles faster, runs (slightly) slower.", EntryFlag::Restart},
                v.disable_spirv_optimizer);

    t.Group("Shaders");
    t << Toggle({"Hardware Shader",
                 "Runs PICA vertex shaders on the GPU. Turning this off is much slower.",
                 EntryFlag::None, QuickSection::Graphics},
                v.use_hw_shader);
    t << Toggle({"Accurate Shader Multiplication",
                 "Matches the PICA's multiply edge cases. Fixes geometry bugs, costs speed."},
                v.shaders_accurate_mul);
    t << Toggle({"Shader JIT",
                 "Compiles CPU-side PICA shaders to native code instead of interpreting them."},
                v.use_shader_jit);
    t << Toggle({"Async Shader Compilation",
                 "Builds shaders on a background thread. Cuts hitching at the cost of one-time visual bugs."},
                v.async_shader_compilation);
    t << Toggle({"Disk Shader Cache",
                 "Keeps compiled shaders on the SD card so later runs hitch less."},
                v.use_disk_shader_cache);
    t << ClearShaderCacheEntry();

    t.Group("Threading");
    t << Toggle({"Async GPU", "Runs PICA and renderer work on their own host thread.",
                 EntryFlag::Restart},
                v.async_gpu_emulation);
    t << Toggle({"Strict GPU Sync", "Drains the GPU queue after every trigger. For debugging."},
                v.strict_gpu_sync);
    t << Toggle({"Async Presentation", "Hands finished frames to the display from another thread."},
                v.async_presentation);

    t.Group("Presentation");
    t << Toggle({"VSync", "Waits for the display before presenting, which removes tearing."},
                v.use_vsync);
    t << Toggle({"Detect Display Refresh Rate",
                 "Reads the panel's real refresh rate instead of assuming 60 Hz."},
                v.use_display_refresh_rate_detection);
    t << Toggle({"Skip Duplicate Frames", "Skips presenting a frame the game did not redraw."},
                v.use_skip_duplicate_frames);

#ifdef ENABLE_LSFG
    t.Group("Frame Generation");
    t << FrameGenerationEntry();
    t << LosslessDllEntry();
    t << Number({"Frame Gen Max Multiplier",
                 "Ceiling on frames presented per rendered frame.", EntryFlag::None,
                 QuickSection::Graphics},
                v.frame_generation_multiplier, 2, 4, 1, "x");
    t << Toggle({"Frame Gen Above 60 Hz",
                 "Lets frame generation exceed 60 fps. Only useful on an overclocked display."},
                v.frame_generation_high_refresh);
    t << Toggle({"Frame Gen Performance Mode",
                 "Trades interpolation quality for a cheaper chain."},
                v.frame_generation_performance_mode);
    t << Number({"Frame Gen Flow Scale",
                 "Optical-flow resolution. Cost grows with the square of this."},
                v.frame_generation_flow_scale, 12, 100, 1, "%");
#endif

    t.Group("Accuracy");
    t << Toggle({"Simulate 3DS GPU Timings",
                 "Makes draws take as long as they would on hardware. Fixes some timing bugs."},
                v.simulate_3ds_gpu_timings);
    t << Number({"Render Thread Delay",
                 "Holds the game's render thread back, for games that outrun their own GPU."},
                v.delay_game_render_thread_us, 0, 10000, 100, " us");
}

void BuildEnhancements(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::Enhancements};
    t.Ini("Renderer");

    t.Group("Resolution");
    t << ResolutionEntry();
    t << Relayout(Toggle({"Integer Scaling",
                          "Only scales the screens by whole numbers (sharper, with letterboxing).",
                          EntryFlag::None, QuickSection::Graphics},
                         v.use_integer_scaling));

    t.Group("Filtering");
    t << Toggle({"Linear Filtering", "Samples the 3DS screens smoothly rather than blockily.",
                 EntryFlag::None, QuickSection::Graphics},
                v.filter_mode);
    t << Choice({"Texture Filter", "Upscales game textures. Anything but None costs GPU time.",
                 EntryFlag::None, QuickSection::Graphics},
                v.texture_filter, kTextureFilterNames);
    t << Choice({"Texture Sampling", "Overrides how the game asked its textures to be sampled.",
                 EntryFlag::None, QuickSection::Graphics},
                v.texture_sampling, kTextureSamplingNames);
    t << Choice({"Anisotropic Filtering", "Sharpens textures viewed at a steep angle.",
                 EntryFlag::None, QuickSection::Graphics},
                v.anisotropic_filtering, kAnisotropyNames);

    t.Group("Custom Textures");
    t.Ini("Utility");
    t << Toggle({"Custom Textures", "Loads a texture pack from load/textures/<title id>/.",
                 EntryFlag::None, QuickSection::Graphics},
                v.custom_textures);
    t << Toggle({"Preload Custom Textures",
                 "Loads the whole pack at boot. Costs memory, avoids in-game hitches."},
                v.preload_textures);
    t << Toggle({"Async Custom Texture Loading", "Decodes pack textures on background threads."},
                v.async_custom_loading);
    t << Toggle({"Dump Textures", "Writes the game's textures out so a pack can be built.",
                 EntryFlag::Restart},
                v.dump_textures);
}

void BuildStereo3D(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::Stereo3D};

    t.Group("Stereoscopic 3D");
    t.Ini("Renderer");
    t << StereoModeEntry();
    t << StereoDepthEntry();
    t << Toggle({"Swap Eyes", "Sends each eye's image to the other eye.", EntryFlag::None,
                 QuickSection::Stereo},
                v.swap_eyes_3d);
    t << Choice({"Eye Rendered In 2D", "Which eye is shown when 3D is off.", EntryFlag::None,
                 QuickSection::Stereo},
                v.mono_render_option, kMonoEyeNames);
    t << DisableRightEyeEntry();

    t.Group("Nintendo Labo VR");
    t.Ini("Layout");
    t << Relayout(Number({"Labo VR Image Size", "Size of each eye's image in the headset.",
                          EntryFlag::None, QuickSection::Stereo},
                         v.cardboard_screen_size, 30, 100, 5, "%"));
    t << Relayout(WithDefault(Number({"Labo VR Horizontal Align",
                                      "Shifts both eyes sideways to line up with the lenses.",
                                      EntryFlag::None, QuickSection::Stereo},
                                     v.cardboard_x_shift, -100, 100, 5, "%"),
                              "35"));
    t << Relayout(Number({"Labo VR Vertical Align",
                          "Shifts both eyes up or down to line up with the lenses.",
                          EntryFlag::None, QuickSection::Stereo},
                         v.cardboard_y_shift, -100, 100, 5, "%"));
}

void BuildAudio(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::Audio};
    t.Ini("Audio");

    t.Group("Output");
    t << Choice({"Audio Emulation", "HLE is fast, LLE runs the real DSP firmware and is accurate.",
                 EntryFlag::Restart},
                v.audio_emulation, kAudioEmulationNames);
    t << Percent({"Volume", "Output volume.", EntryFlag::None, QuickSection::Audio}, v.volume, 0,
                 100, 5);
    t << Toggle({"Simulate Headphones", "Tells the game headphones are plugged in.",
                 EntryFlag::None, QuickSection::Audio},
                v.simulate_headphones_plugged);

    t.Group("Timing");
    t << AudioStretchingEntry();
    t << Toggle({"Realtime Audio", "Runs the audio mixer at a higher thread priority."},
                v.enable_realtime_audio);
}

void BuildLayout(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::Layout};

    t.Group("Screens");
    t << ScreenLayoutEntry();
    t << SwapScreensEntry();
    t.Ini("Layout");
    t << Relayout(Number({"Screen Gap", "Space left between the two screens.", EntryFlag::None,
                          QuickSection::Display},
                         v.screen_gap, 0, 200, 4, " px"));
    t << LargeScreenProportionEntry();
    t.Ini("Switch");
    t << LocalBool("stretch_fullscreen",
                   {"Stretch Fullscreen",
                    "Fills the whole display in the single-screen layouts.", EntryFlag::None,
                    QuickSection::Display},
                   IsFullscreenStretchEnabled, SetFullscreenStretchEnabled, false);

    t.Group("Screen Overlay");
    t.Ini("Layout");
    t << Relayout(Choice({"Overlay Screen Position",
                          "Where the small screen sits inside the big one.", EntryFlag::None,
                          QuickSection::Display},
                         v.overlay_screen_position, kOverlayPositionNames));
    t << Relayout(Number({"Overlay Screen Size", "Width of the overlaid screen.", EntryFlag::None,
                          QuickSection::Display},
                         v.overlay_screen_size, 10, 60, 5, "%"));
    t << Relayout(Number({"Overlay Screen Opacity", "How solid the overlaid screen is drawn.",
                          EntryFlag::None, QuickSection::Display},
                         v.overlay_screen_opacity, 10, 100, 10, "%"));

    t.Group("Padding");
    t << Relayout(Number({"Top Screen Padding X", "Blank space beside the top screen."},
                         v.screen_top_leftright_padding, 0, 200, 4, " px"));
    t << Relayout(Number({"Top Screen Padding Y", "Blank space above and below the top screen."},
                         v.screen_top_topbottom_padding, 0, 200, 4, " px"));
    t << Relayout(Number({"Bottom Screen Padding X", "Blank space beside the bottom screen."},
                         v.screen_bottom_leftright_padding, 0, 200, 4, " px"));
    t << Relayout(Number({"Bottom Screen Padding Y",
                          "Blank space above and below the bottom screen."},
                         v.screen_bottom_topbottom_padding, 0, 200, 4, " px"));

    t.Group("Background");
    t << ColorChannel({"Background Red", "Red channel of the colour behind the screens."},
                      v.bg_red);
    t << ColorChannel({"Background Green", "Green channel of the colour behind the screens."},
                      v.bg_green);
    t << ColorChannel({"Background Blue", "Blue channel of the colour behind the screens."},
                      v.bg_blue);

    t.Group("Menu");
    t.Ini("Switch");
    t << LocalNumber(
        "menu_rotation",
        {"Menu Rotation", "Turns the launcher and the in-game overlay on their side.",
         EntryFlag::None, QuickSection::Display},
        GetMenuRotation, SetMenuRotation, 0, 0, 270, 90, " deg");
    t << LocalBool("menu_rotate_input",
                   {"Rotate Menu Input",
                    "Turns the d-pad with the menu, for holding the console sideways.",
                    EntryFlag::None, QuickSection::Display},
                   IsMenuInputRotated, SetMenuInputRotated, false);
    t << LayoutCycleEntry();
}

void BuildControls(std::vector<SettingEntry>& out) {
    Table t{out, Category::Controls};
    t.Ini("Controls");

    t.Group("Buttons");
    t << Modal({"Controller Mapping", "Which Switch button drives each 3DS control."},
               SettingsModal::ControllerMap, [] { return std::string{"Configure"}; });
    t << Toggle({"Use Artic Base Controller",
                 "Takes input from the real 3DS while connected over Artic Base."},
                Settings::values.use_artic_base_controller);

    t.Group("Touch Pointer");
    t.Ini("Switch");
    t << PointerModeEntry();
    t << LocalEnum(
        "pointer_source",
        {"Touch Pointer Source", "What moves the pointer across the 3DS touch screen.",
         EntryFlag::None, QuickSection::Input},
        [] { return static_cast<int>(GetPointerSource()); },
        [](int value) { SetPointerSource(static_cast<PointerSource>(value)); }, 0,
        NumPointerSources,
        [](int value) { return PointerSourceName(static_cast<PointerSource>(value)); });

    t.Group("Motion");
    t << LocalEnum(
        "gyro_source", {"Gyro Source", "Which controller's motion sensor is read.",
                        EntryFlag::None, QuickSection::Input},
        [] { return static_cast<int>(GetGyroSource()); },
        [](int value) { SetGyroSource(static_cast<GyroSource>(value)); }, 0, NumGyroSources,
        [](int value) { return GyroSourceName(static_cast<GyroSource>(value)); });
    t << LocalEnum(
        "gyro_orientation",
        {"Gyro Orientation", "Which way up the console is held, so motion maps correctly.",
         EntryFlag::None, QuickSection::Input},
        [] { return static_cast<int>(GetGyroOrientation()); },
        [](int value) { SetGyroOrientation(static_cast<GyroOrientation>(value)); }, 0,
        NumGyroOrientations,
        [](int value) { return GyroOrientationName(static_cast<GyroOrientation>(value)); });
    t << GyroSensitivityEntry(true);
    t << GyroSensitivityEntry(false);
}

void BuildSystem(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::System};

    t.Group("Console");
    t.Ini("Core");
    t << WithDefault(Toggle({"New 3DS Mode",
                             "Emulates a New 3DS. Doubles the emulated cores, costs performance in "
                             "games that do not need them.",
                             EntryFlag::Restart},
                            v.is_new_3ds),
                     "false");
    t.Ini("System");
    t << RegionEntry();
    t << Toggle({"Region Free Patch", "Patches out the region check so imports will boot."},
                v.apply_region_free_patch);

    t.Group("Profile");
    t << Modal({"Username", "Name the emulated console answers to."}, SettingsModal::Username,
               [] {
                   const std::string name = GetProfileUsername();
                   return name.empty() ? std::string{"Not set"} : name;
               });
    t << BirthMonthEntry();
    t << BirthDayEntry();
    t << ProfileChoice({"System Language", "Language games ask the console for."},
                       ProfileValue::Language, kLanguageNames);
    t << Modal({"Country", "Country the console reports. Games check this against the region."},
               SettingsModal::Country, [] {
                   const std::string name{ProfileCountryName()};
                   return IsCountryValidForRegion(GetProfileCountry()) ? name
                                                                      : name + " (wrong region)";
               });
    t << ProfileChoice({"Sound Output", "Speaker arrangement the console reports."},
                       ProfileValue::SoundMode, kSoundOutputNames);
    t << PlayCoinsEntry();
    t << SystemSetupEntry();

    t.Group("Clock");
    t << Choice({"Clock Source", "Where the emulated clock starts from."}, v.init_clock,
                kInitClockNames);
    t << PersistedModal({"Fixed Clock Time", "The date and time a fixed clock starts at."},
                        SettingsModal::FixedClock, v.init_time, GetFixedClockText);
    t << ClockOffsetEntry();
    t << Choice({"Initial Ticks", "What the CPU tick counter starts at."}, v.init_ticks_type,
                kInitTicksNames);
    t << PersistedModal({"Initial Ticks Value", "The tick count a fixed start uses."},
                        SettingsModal::InitTicksValue, v.init_ticks_override, GetInitTicksText);

    t.Group("Modules");
    t << Toggle({"LLE Applets", "Runs the console's own applets instead of the built-in ones.",
                 EntryFlag::Restart},
                v.lle_applets);
    t << Toggle({"Required Online LLE Modules",
                 "Loads the real sysmodules some online games insist on.", EntryFlag::Restart},
                v.enable_required_online_lle_modules);

    t.Group("Plugins");
    t << Toggle({"Plugin Loader", "Loads 3GX plugins for the running game.", EntryFlag::Restart},
                v.plugin_loader_enabled);
    t << Toggle({"Allow Games To Change Plugin Loader",
                 "Lets the game turn the plugin loader on and off itself."},
                v.allow_plugin_loader);

    t.Group("Pedometer");
    t << Number({"Pedometer Steps Per Hour", "Steps the console pretends you walked each hour."},
                v.steps_per_hour, 0, 10000, 100);
}

void BuildConsole(std::vector<SettingEntry>& out) {
    Table t{out, Category::Console};
    t.Ini("Debugging");

    t.Group("Identity");
    t << Modal({"Console ID", "The unique ID this console reports. Regenerating changes it."},
               SettingsModal::ConsoleId, GetConsoleIdText);
    t << Modal({"MAC Address", "The network address this console reports."},
               SettingsModal::MacAddress, GetMacAddressText);
    t << Toggle({"Unique Data Console Type",
                 "Reads the console model out of the installed unique data."},
                Settings::values.toggle_unique_data_console_type);

    t.Group("Unique Data");
    t << UniqueDataEntry(UniqueDataFile::SecureInfo, "Region and serial dumped from a real 3DS.");
    t << UniqueDataEntry(UniqueDataFile::FriendCodeSeed, "Friend code seed dumped from a real 3DS.");
    t << UniqueDataEntry(UniqueDataFile::Otp, "One-time programmable block dumped from a real 3DS.");
    t << UniqueDataEntry(UniqueDataFile::Movable, "SD key material dumped from a real 3DS.");
    t << Modal({"Unlink Console", "Removes the dumped files and stops impersonating that console."},
               SettingsModal::UnlinkConsole,
               [] { return std::string{IsConsoleLinked() ? "Linked" : "Not linked"}; });
}

void BuildStorage(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::Storage};
    t.Ini("Data Storage");

    t.Group("SD Card");
    t << Toggle({"Virtual SD Card", "Gives the emulated console an SD card at all."},
                v.use_virtual_sd);
    t << Toggle({"Async Filesystem Operations",
                 "Runs guest file reads and writes off the emulated CPU thread."},
                v.async_fs_operations);

    t.Group("Installs");
    t << Toggle({"Compress CIA Installs", "Shrinks installed titles on disk at some CPU cost."},
                v.compress_cia_installs);
}

void BuildAdvanced(std::vector<SettingEntry>& out) {
    auto& v = Settings::values;
    Table t{out, Category::Advanced};

    t.Group("CPU");
    t.Ini("Core");
    t << Toggle({"CPU JIT", "Compiles guest ARM11 code. Turning this off is far slower.",
                 EntryFlag::Restart},
                v.use_cpu_jit);
    t << Toggle({"Fastmem", "Maps guest memory straight into the JIT's address space.",
                 EntryFlag::Restart},
                v.fastmem);

    t.Group("Speed Hacks");
    t.Ini("Experimental");
    t << LocalBool("movie_cpu_throttle",
                   {"Movie CPU Throttle", "Drops the emulated clock while a cutscene is playing."},
                   IsMovieThrottleEnabled, SetMovieThrottleEnabled, false);
    t << LocalNumber(
        "movie_cpu_clock_percentage",
        {"Movie CPU Clock", "The clock the movie throttle drops to."},
        [] { return static_cast<int>(GetMovieThrottleClockPercentage()); },
        [](int value) { SetMovieThrottleClockPercentage(static_cast<std::int32_t>(value)); }, 45,
        10, 100, 5, "%");
    t << Toggle({"Skip Slow Draw", "Drops draws that cannot take the accelerated vertex path."},
                v.skip_slow_draw);
    t << Toggle({"Skip Texture Copy", "Drops texture copies whose source the GPU has not cached."},
                v.skip_texture_copy);
    t << Toggle({"Skip CPU Write", "Keeps cached GPU surfaces through tiny CPU writes."},
                v.skip_cpu_write);

    t.Group("Debug");
    t.Ini("Debugging");
    t << Toggle({"Deterministic Async Operations",
                 "Makes background work finish in a fixed order, for reproducing bugs."},
                v.deterministic_async_operations);
    t << Toggle({"Delay Start For LLE Modules", "Waits for the real sysmodules to come up first."},
                v.delay_start_for_lle_modules);
    t << Toggle({"Log Memory Exceptions",
                 "Dumps guest CPU state when the game walks a bad pointer."},
                v.enable_exception_handler);
    t << Toggle({"Renderer Debug", "Turns on the graphics backend's validation layers.",
                 EntryFlag::Restart},
                v.renderer_debug);
    t << Toggle({"Dump Command Buffers", "Writes the GPU command stream out to disk."},
                v.dump_command_buffers);

    t.Group("Logging");
    t << Toggle({"Instant Debug Log", "Flushes every log line immediately. Very slow."},
                v.instant_debug_log);
    t.Ini("Miscellaneous");
    t << PersistedModal({"Log Filter", "Which subsystems log, and at what level."},
                        SettingsModal::LogFilter, v.log_filter,
                        [] { return Settings::values.log_filter.GetValue(); });
}

std::vector<SettingEntry> BuildRegistry() {
    std::vector<SettingEntry> entries;
    entries.reserve(160);
    BuildGeneral(entries);
    BuildGraphics(entries);
    BuildEnhancements(entries);
    BuildStereo3D(entries);
    BuildAudio(entries);
    BuildLayout(entries);
    BuildControls(entries);
    BuildSystem(entries);
    BuildConsole(entries);
    BuildStorage(entries);
    BuildAdvanced(entries);
    return entries;
}

std::string ToLower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

} // namespace

const char* CategoryName(Category category) {
    switch (category) {
    case Category::General:
        return "General";
    case Category::Graphics:
        return "Graphics";
    case Category::Enhancements:
        return "Enhancements";
    case Category::Stereo3D:
        return "3D";
    case Category::Audio:
        return "Audio";
    case Category::Layout:
        return "Layout";
    case Category::Controls:
        return "Controls";
    case Category::System:
        return "System";
    case Category::Console:
        return "Console";
    case Category::Storage:
        return "Storage";
    case Category::Advanced:
        return "Advanced";
    default:
        return "";
    }
}

const char* QuickSectionName(QuickSection section) {
    switch (section) {
    case QuickSection::Display:
        return "Display";
    case QuickSection::Graphics:
        return "Graphics";
    case QuickSection::Stereo:
        return "3D";
    case QuickSection::Audio:
        return "Audio";
    case QuickSection::Input:
        return "Input";
    case QuickSection::System:
        return "System";
    default:
        return "";
    }
}

const std::vector<SettingEntry>& Registry() {
    static const std::vector<SettingEntry> entries = BuildRegistry();
    return entries;
}

std::vector<const SettingEntry*> EntriesIn(Category category) {
    std::vector<const SettingEntry*> out;
    for (const SettingEntry& entry : Registry()) {
        if (entry.category == category) {
            out.push_back(&entry);
        }
    }
    return out;
}

std::vector<const SettingEntry*> EntriesInQuick(QuickSection section) {
    std::vector<const SettingEntry*> out;
    for (const SettingEntry& entry : Registry()) {
        if (entry.quick == section && entry.IsShownInQuickMenu()) {
            out.push_back(&entry);
        }
    }
    return out;
}

std::vector<const SettingEntry*> SearchEntries(std::string_view query) {
    std::vector<std::string> terms;
    std::istringstream stream{ToLower(query)};
    for (std::string term; stream >> term;) {
        terms.push_back(term);
    }
    if (terms.empty()) {
        return {};
    }

    std::vector<std::pair<int, const SettingEntry*>> ranked;
    for (const SettingEntry& entry : Registry()) {
        const std::string label = ToLower(entry.label);
        const std::string context =
            ToLower(entry.subgroup) + ' ' + ToLower(CategoryName(entry.category));
        const std::string id = ToLower(entry.id != nullptr ? entry.id : "");
        int rank = 0;
        bool matched = true;
        for (const std::string& term : terms) {
            if (label.find(term) != std::string::npos) {
                continue;
            }
            if (context.find(term) != std::string::npos) {
                rank = std::max(rank, 1);
                continue;
            }
            if (id.find(term) != std::string::npos) {
                rank = std::max(rank, 2);
                continue;
            }
            matched = false;
            break;
        }
        if (matched) {
            ranked.push_back({rank, &entry});
        }
    }

    std::stable_sort(ranked.begin(), ranked.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    std::vector<const SettingEntry*> out;
    out.reserve(ranked.size());
    for (const auto& [rank, entry] : ranked) {
        out.push_back(entry);
    }
    return out;
}

std::vector<const SettingEntry*> OverridableEntriesIn(Category category) {
    std::vector<const SettingEntry*> out;
    for (const SettingEntry* entry : EntriesIn(category)) {
        if (entry->IsOverridable()) {
            out.push_back(entry);
        }
    }
    return out;
}

std::vector<const SettingEntry*> SearchOverridableEntries(std::string_view query) {
    std::vector<const SettingEntry*> out;
    for (const SettingEntry* entry : SearchEntries(query)) {
        if (entry->IsOverridable()) {
            out.push_back(entry);
        }
    }
    return out;
}

bool CategoryHasOverridables(Category category) {
    for (const SettingEntry& entry : Registry()) {
        if (entry.category == category && entry.IsOverridable()) {
            return true;
        }
    }
    return false;
}

void RestoreGlobalSettings() {
    for (const SettingEntry& entry : Registry()) {
        if (entry.set_global) {
            entry.set_global(true);
        }
    }
}

const SettingEntry* FindEntry(std::string_view id) {
    for (const SettingEntry& entry : Registry()) {
        if (entry.id != nullptr && id == entry.id) {
            return &entry;
        }
    }
    return nullptr;
}

void ResetEntriesToDefaults() {
    for (const SettingEntry& entry : Registry()) {
        if (entry.IsPersisted()) {
            entry.load(entry.default_text());
        }
    }
}

const char* ActiveGraphicsBackendName() {
    return GraphicsApiName(Settings::GetWorkingGraphicsAPI());
}

void RefreshShaderCacheSize() {
    s_shader_cache_size = GetShaderCacheSize();
}

void RefreshUniqueDataStatus() {
    for (std::size_t i = 0; i < s_unique_data_status.size(); ++i) {
        s_unique_data_status[i] = UniqueDataStatus(static_cast<UniqueDataFile>(i));
    }
}

} // namespace SwitchFrontend
