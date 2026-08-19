// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "audio_core/dsp_interface.h"
#include "citra_switch/config.h"
#include "citra_switch/input.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/overlay_menu.h"
#include "citra_switch/settings_menu.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/ptm/ptm.h"
#include "core/hw/unique_data.h"

namespace SwitchFrontend {

namespace {

std::string BoolText(bool on) {
    return on ? "On" : "Off";
}

// Ordered to match Service::CFG::SystemLanguage.
constexpr std::array<const char*, 12> kLanguageNames{
    "Japanese",           "English", "French", "German",     "Italian", "Spanish",
    "Simplified Chinese", "Korean",  "Dutch",  "Portuguese", "Russian", "Traditional Chinese"};

// Ordered to match the SMDH region list, offset by one so index 0 is the auto-select sentinel.
constexpr std::array<const char*, 8> kRegionNames{"Auto",      "Japan", "USA",   "Europe",
                                                  "Australia", "China", "Korea", "Taiwan"};

// Ordered to match Service::CFG::SoundOutputMode.
constexpr std::array<const char*, 3> kSoundOutputNames{"Mono", "Stereo", "Surround"};

// Ordered to match Settings::InitTicks.
constexpr std::array<const char*, 2> kInitTicksNames{"Random", "Fixed"};

constexpr std::array<const char*, 12> kMonthNames{"January",   "February", "March",    "April",
                                                  "May",       "June",     "July",     "August",
                                                  "September", "October",  "November", "December"};

// February is given 29 so a leap-year birthday can be entered.
constexpr std::array<int, 12> kDaysInMonth{31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

// Indexed by the 3DS country code. The gaps are codes the console does not assign.
constexpr std::array<const char*, 187> kCountryNames{
    "",
    "Japan",
    "",
    "",
    "",
    "",
    "",
    "",
    "Anguilla",
    "Antigua and Barbuda",
    "Argentina",
    "Aruba",
    "Bahamas",
    "Barbados",
    "Belize",
    "Bolivia",
    "Brazil",
    "British Virgin Islands",
    "Canada",
    "Cayman Islands",
    "Chile",
    "Colombia",
    "Costa Rica",
    "Dominica",
    "Dominican Republic",
    "Ecuador",
    "El Salvador",
    "French Guiana",
    "Grenada",
    "Guadeloupe",
    "Guatemala",
    "Guyana",
    "Haiti",
    "Honduras",
    "Jamaica",
    "Martinique",
    "Mexico",
    "Montserrat",
    "Netherlands Antilles",
    "Nicaragua",
    "Panama",
    "Paraguay",
    "Peru",
    "Saint Kitts and Nevis",
    "Saint Lucia",
    "Saint Vincent and the Grenadines",
    "Suriname",
    "Trinidad and Tobago",
    "Turks and Caicos Islands",
    "United States",
    "Uruguay",
    "US Virgin Islands",
    "Venezuela",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "Albania",
    "Australia",
    "Austria",
    "Belgium",
    "Bosnia and Herzegovina",
    "Botswana",
    "Bulgaria",
    "Croatia",
    "Cyprus",
    "Czech Republic",
    "Denmark",
    "Estonia",
    "Finland",
    "France",
    "Germany",
    "Greece",
    "Hungary",
    "Iceland",
    "Ireland",
    "Italy",
    "Latvia",
    "Lesotho",
    "Liechtenstein",
    "Lithuania",
    "Luxembourg",
    "Macedonia",
    "Malta",
    "Montenegro",
    "Mozambique",
    "Namibia",
    "Netherlands",
    "New Zealand",
    "Norway",
    "Poland",
    "Portugal",
    "Romania",
    "Russia",
    "Serbia",
    "Slovakia",
    "Slovenia",
    "South Africa",
    "Spain",
    "Swaziland",
    "Sweden",
    "Switzerland",
    "Turkey",
    "United Kingdom",
    "Zambia",
    "Zimbabwe",
    "Azerbaijan",
    "Mauritania",
    "Mali",
    "Niger",
    "Chad",
    "Sudan",
    "Eritrea",
    "Djibouti",
    "Somalia",
    "Andorra",
    "Gibraltar",
    "Guernsey",
    "Isle of Man",
    "Jersey",
    "Monaco",
    "Taiwan",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "South Korea",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "Hong Kong",
    "Macau",
    "",
    "",
    "",
    "",
    "",
    "",
    "Indonesia",
    "Singapore",
    "Thailand",
    "Philippines",
    "Malaysia",
    "",
    "",
    "",
    "China",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
    "United Arab Emirates",
    "India",
    "Egypt",
    "Oman",
    "Qatar",
    "Kuwait",
    "Saudi Arabia",
    "Syria",
    "Bahrain",
    "Jordan",
    "",
    "",
    "",
    "",
    "",
    "",
    "San Marino",
    "Vatican City",
    "Bermuda",
};

// The profile data is kept in the CFG NAND savegame rather than config.ini.
struct Profile {
    std::u16string username;
    int birth_month{1};
    int birth_day{1};
    int language{};
    int sound_mode{};
    int country{};
    bool system_setup{};
};

std::shared_ptr<Service::CFG::Module> s_cfg;
Profile s_profile;
Profile s_profile_saved;
bool s_profile_loaded = false;

std::optional<int> s_play_coins;

Service::CFG::Module& Cfg() {
    if (!s_cfg) {
        s_cfg = Service::CFG::GetModule(Core::System::GetInstance());
    }
    return *s_cfg;
}

Profile& GetProfile() {
    if (!s_profile_loaded) {
        auto& cfg = Cfg();
        const auto [month, day] = cfg.GetBirthday();
        s_profile.username = cfg.GetUsername();
        s_profile.birth_month = std::clamp<int>(month, 1, 12);
        s_profile.birth_day = std::clamp<int>(day, 1, kDaysInMonth[s_profile.birth_month - 1]);
        s_profile.language = static_cast<int>(cfg.GetSystemLanguage());
        s_profile.sound_mode = static_cast<int>(cfg.GetSoundOutputMode());
        s_profile.country = cfg.GetCountryCode();
        s_profile.system_setup = cfg.IsSystemSetupNeeded();
        s_profile_saved = s_profile;
        s_profile_loaded = true;
    }
    return s_profile;
}

int ReadPlayCoins() {
    if (!s_play_coins) {
        s_play_coins = Service::PTM::Module::GetPlayCoins();
    }
    return *s_play_coins;
}

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

constexpr std::array<const char*, 2> kInitClockNames{"System time", "Fixed time"};

// Matches Settings::SmallScreenPosition. AboveLarge/BelowLarge anchor to the centre when the
// layout floats one screen over the other.
constexpr std::array<const char*, 8> kOverlayPositionNames{
    "Top right",   "Middle right", "Bottom right", "Top left",
    "Middle left", "Bottom left",  "Top centre",   "Bottom centre"};

// The backends this build can actually switch between.
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

template <typename S>
using ValueOf = std::decay_t<decltype(std::declval<S&>().GetValue())>;

template <typename S>
SettingsRow Toggle(const char* label, S& setting) {
    const auto get = [&setting] { return static_cast<bool>(setting.GetValue()); };
    return {label, [get] { return BoolText(get()); }, [&setting](int dir) { setting = dir > 0; },
            SettingsModal::None, get};
}

// A boolean held by the frontend rather than by Settings::values.
SettingsRow BoolRow(const char* label, bool (*get)(), void (*set)(bool)) {
    return {label, [get] { return BoolText(get()); }, [set](int dir) { set(dir > 0); },
            SettingsModal::None, [get] { return get(); }};
}

// Wraps a row whose setting feeds the framebuffer layout, which the emulation thread only
// recomputes when asked.
SettingsRow Relayout(SettingsRow row) {
    row.step = [step = std::move(row.step)](int dir) {
        step(dir);
        RequestLayoutUpdate();
    };
    return row;
}

// An integer setting stepped between `lo` and `hi`, rendered as "<value><suffix>".
template <typename S>
SettingsRow Number(const char* label, S& setting, int lo, int hi, int step,
                   const char* suffix = "") {
    return {label,
            [&setting, suffix] {
                return std::to_string(static_cast<int>(setting.GetValue())) + suffix;
            },
            [&setting, lo, hi, step](int dir) {
                setting = static_cast<ValueOf<S>>(
                    std::clamp(static_cast<int>(setting.GetValue()) + dir * step, lo, hi));
            }};
}

// An enum setting cycled through `names`, which is indexed by the underlying value.
template <typename S, std::size_t N>
SettingsRow Choice(const char* label, S& setting, const std::array<const char*, N>& names) {
    return {label,
            [&setting, &names] {
                const auto index = static_cast<std::size_t>(setting.GetValue());
                return std::string{index < names.size() ? names[index] : "Unknown"};
            },
            [&setting, &names](int dir) {
                setting = static_cast<ValueOf<S>>(std::clamp(
                    static_cast<int>(setting.GetValue()) + dir, 0, static_cast<int>(N) - 1));
            }};
}

SettingsRow StereoModeRow() {
    SettingsRow row = Choice("Stereoscopic 3D", Settings::values.render_3d, kStereoNames);
    row.step = [step = std::move(row.step)](int dir) {
        step(dir);
        if (Settings::values.render_3d.GetValue() != Settings::StereoRenderOption::Off) {
            // A zeroed 3DS depth slider produces two identical images, which makes a newly enabled
            // headset mode look broken.
            if (Settings::values.factor_3d.GetValue() == 0) {
                Settings::values.factor_3d = 60;
            }
            Settings::values.disable_right_eye_render = false;
        }
    };
    return Relayout(std::move(row));
}

// Config load zeroes the depth slider whenever stereo is off.
SettingsRow StereoDepthRow() {
    SettingsRow row = Number("Stereoscopic Depth", Settings::values.factor_3d, 0, 100, 5, "%");
    const auto stereo_is_off = [] {
        return Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::Off;
    };
    row.value = [value = std::move(row.value), stereo_is_off] {
        return stereo_is_off() ? std::string{"Needs 3D"} : value();
    };
    row.step = [step = std::move(row.step), stereo_is_off](int dir) {
        if (!stereo_is_off()) {
            step(dir);
        }
    };
    return row;
}

SettingsRow DisableRightEyeRow() {
    auto& setting = Settings::values.disable_right_eye_render;
    const auto stereo_is_off = [] {
        return Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::Off;
    };
    return {
        "Disable Right Eye Render",
        [&setting, stereo_is_off] {
            return stereo_is_off() ? BoolText(setting.GetValue()) : std::string{"Required for 3D"};
        },
        [&setting, stereo_is_off](int dir) {
            if (stereo_is_off()) {
                setting = dir > 0;
            }
        },
        SettingsModal::None,
        [&setting, stereo_is_off] { return stereo_is_off() && setting.GetValue(); },
    };
}

// A 0..1 float shown as a whole-number percentage.
template <typename S>
SettingsRow Percent(const char* label, S& setting, int lo, int hi, int step) {
    const auto as_percent = [](float value) {
        return static_cast<int>(std::lround(value * 100.0f));
    };
    return {label,
            [&setting, as_percent] { return std::to_string(as_percent(setting.GetValue())) + "%"; },
            [&setting, as_percent, lo, hi, step](int dir) {
                setting = static_cast<float>(
                              std::clamp(as_percent(setting.GetValue()) + dir * step, lo, hi)) /
                          100.0f;
            }};
}

// A 0..1 float shown as an 8-bit colour channel.
SettingsRow ColorChannel(const char* label, Settings::SwitchableSetting<float>& setting) {
    const auto as_byte = [](float value) { return static_cast<int>(std::lround(value * 255.0f)); };
    return {label, [&setting, as_byte] { return std::to_string(as_byte(setting.GetValue())); },
            [&setting, as_byte](int dir) {
                setting =
                    static_cast<float>(std::clamp(as_byte(setting.GetValue()) + dir * 8, 0, 255)) /
                    255.0f;
            }};
}

SettingsRow GraphicsApiRow() {
    auto& setting = Settings::values.graphics_api;
    return {"Graphics API (restart)",
            [&setting] { return std::string{GraphicsApiName(setting.GetValue())}; },
            [&setting](int dir) {
                const auto it =
                    std::find(kGraphicsApis.begin(), kGraphicsApis.end(), setting.GetValue());
                const int current =
                    it == kGraphicsApis.end() ? 0 : static_cast<int>(it - kGraphicsApis.begin());
                setting = kGraphicsApis[std::clamp(current + dir, 0,
                                                   static_cast<int>(kGraphicsApis.size()) - 1)];
            }};
}

SettingsRow FrameLimitRow() {
    auto& setting = Settings::values.frame_limit;
    return {"Frame Limit",
            [&setting] {
                const int limit = static_cast<int>(setting.GetValue());
                return limit == 0 ? std::string{"Off"} : std::to_string(limit) + "%";
            },
            [&setting](int dir) {
                setting = static_cast<double>(
                    std::clamp(static_cast<int>(setting.GetValue()) + dir * 5, 0, 1000));
            }};
}

SettingsRow ResolutionRow() {
    auto& setting = Settings::values.resolution_factor;
    return {"Internal Resolution",
            [&setting] {
                switch (setting.GetValue()) {
                case 0:
                    return std::string{"Auto (window)"};
                case 1:
                    return std::string{"Native (1x)"};
                default:
                    return std::to_string(setting.GetValue()) + "x";
                }
            },
            [&setting](int dir) {
                setting =
                    static_cast<u32>(std::clamp(static_cast<int>(setting.GetValue()) + dir, 0, 4));
            }};
}

SettingsRow RegionRow() {
    auto& setting = Settings::values.region_value;
    return {"Console Region",
            [&setting] {
                const auto index = static_cast<std::size_t>(setting.GetValue() + 1);
                return std::string{index < kRegionNames.size() ? kRegionNames[index] : "Auto"};
            },
            [&setting](int dir) {
                setting = static_cast<s32>(std::clamp(setting.GetValue() + dir, -1, 6));
            }};
}

// An integer field of the staged profile stepped through `names`.
template <std::size_t N>
SettingsRow ProfileChoice(const char* label, int Profile::*field,
                          const std::array<const char*, N>& names) {
    return {label,
            [field, &names] {
                const auto index = static_cast<std::size_t>(GetProfile().*field);
                return std::string{index < names.size() ? names[index] : "Unknown"};
            },
            [field, &names](int dir) {
                Profile& profile = GetProfile();
                profile.*field =
                    std::clamp(profile.*field + dir, 0, static_cast<int>(N) - 1);
            }};
}

SettingsRow LanguageRow() {
    return ProfileChoice("System Language", &Profile::language, kLanguageNames);
}

SettingsRow SoundOutputRow() {
    return ProfileChoice("Sound Output", &Profile::sound_mode, kSoundOutputNames);
}

SettingsRow UsernameRow() {
    return {"Username",
            [] {
                const std::string name = Common::UTF16ToUTF8(GetProfile().username);
                return name.empty() ? std::string{"Not set"} : name;
            },
            {},
            SettingsModal::Username};
}

SettingsRow BirthMonthRow() {
    return {"Birthday Month",
            [] {
                const auto index = static_cast<std::size_t>(GetProfile().birth_month - 1);
                return std::string{index < kMonthNames.size() ? kMonthNames[index] : "January"};
            },
            [](int dir) {
                Profile& profile = GetProfile();
                profile.birth_month = std::clamp(profile.birth_month + dir, 1, 12);
                profile.birth_day =
                    std::min(profile.birth_day, kDaysInMonth[profile.birth_month - 1]);
            }};
}

SettingsRow BirthDayRow() {
    return {"Birthday Day", [] { return std::to_string(GetProfile().birth_day); },
            [](int dir) {
                Profile& profile = GetProfile();
                profile.birth_day = std::clamp(profile.birth_day + dir, 1,
                                               kDaysInMonth[profile.birth_month - 1]);
            }};
}

const char* CountryName(int code) {
    const auto index = static_cast<std::size_t>(code);
    if (index >= kCountryNames.size() || kCountryNames[index][0] == '\0') {
        return "Unknown";
    }
    return kCountryNames[index];
}

SettingsRow CountryRow() {
    return {"Country",
            [] {
                const int code = GetProfile().country;
                const std::string name{CountryName(code)};
                return IsCountryValidForRegion(code) ? name : name + " (wrong region)";
            },
            {},
            SettingsModal::Country};
}

SettingsRow SystemSetupRow() {
    const auto get = [] { return GetProfile().system_setup; };
    return {"System Setup Required", [get] { return BoolText(get()); },
            [](int dir) { GetProfile().system_setup = dir > 0; }, SettingsModal::None, get};
}

SettingsRow PlayCoinsRow() {
    return {"Play Coins", [] { return std::to_string(ReadPlayCoins()); },
            [](int dir) { s_play_coins = std::clamp(ReadPlayCoins() + dir, 0, 300); }};
}

// The kernel takes the offset in seconds.
SettingsRow ClockOffsetRow() {
    auto& setting = Settings::values.init_time_offset;
    return {"Clock Offset",
            [&setting] {
                const long long days = setting.GetValue() / 86400;
                return std::to_string(days) + (days == 1 || days == -1 ? " day" : " days");
            },
            [&setting](int dir) {
                const long long days =
                    std::clamp<long long>(setting.GetValue() / 86400 + dir, -3650, 3650);
                setting = static_cast<s64>(days * 86400);
            }};
}

SettingsRow FixedClockRow() {
    return {"Fixed Clock Time", GetFixedClockText, {}, SettingsModal::FixedClock};
}

SettingsRow InitTicksValueRow() {
    return {"Initial Ticks Value", GetInitTicksText, {}, SettingsModal::InitTicksValue};
}

SettingsRow ConsoleIdRow() {
    return {"Console ID", GetConsoleIdText, {}, SettingsModal::ConsoleId};
}

SettingsRow MacAddressRow() {
    return {"MAC Address", GetMacAddressText, {}, SettingsModal::MacAddress};
}

// Reading the status touches the disk, so it is snapshotted when the page is built.
SettingsRow UniqueDataRow(UniqueDataFile file) {
    const auto modal = static_cast<SettingsModal>(
        static_cast<int>(SettingsModal::InstallSecureInfo) + static_cast<int>(file));
    const std::string status = UniqueDataStatus(file);
    return {UniqueDataFileName(file), [status] { return status; }, {}, modal};
}

bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int DaysInMonth(int year, int month) {
    if (month == 2) {
        return IsLeapYear(year) ? 29 : 28;
    }
    return kDaysInMonth[month - 1];
}

// Days since 1970-01-01 since newlib has no timegm().
long long DaysFromCivil(int year, int month, int day) {
    year -= month <= 2 ? 1 : 0;
    const long long era = (year >= 0 ? year : year - 399) / 400;
    const long long year_of_era = year - era * 400;
    const long long day_of_year =
        (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const long long day_of_era =
        year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

std::string UniqueDataPath(UniqueDataFile file) {
    switch (file) {
    case UniqueDataFile::SecureInfo:
        return HW::UniqueData::GetSecureInfoAPath();
    case UniqueDataFile::FriendCodeSeed:
        return HW::UniqueData::GetLocalFriendCodeSeedBPath();
    case UniqueDataFile::Otp:
        return HW::UniqueData::GetOTPPath();
    default:
        return HW::UniqueData::GetMovablePath();
    }
}

HW::UniqueData::SecureDataLoadStatus LoadUniqueData(UniqueDataFile file) {
    switch (file) {
    case UniqueDataFile::SecureInfo:
        return HW::UniqueData::LoadSecureInfoA();
    case UniqueDataFile::FriendCodeSeed:
        return HW::UniqueData::LoadLocalFriendCodeSeedB();
    case UniqueDataFile::Otp:
        return HW::UniqueData::LoadOTP();
    default:
        return HW::UniqueData::LoadMovable();
    }
}

SettingsRow UnlinkConsoleRow() {
    return {"Unlink Console",
            [] { return std::string{IsConsoleLinked() ? "Linked" : "Not linked"}; },
            {},
            SettingsModal::UnlinkConsole};
}

SettingsRow LargeScreenProportionRow() {
    auto& setting = Settings::values.large_screen_proportion;
    return {"Large Screen Proportion",
            [&setting] {
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "%.2fx", setting.GetValue());
                return std::string{buffer};
            },
            [&setting](int dir) {
                setting = std::clamp(setting.GetValue() + dir * 0.25f, 1.0f, 16.0f);
            }};
}

SettingsRow StretchFullscreenRow() {
    return BoolRow("Stretch Fullscreen", IsFullscreenStretchEnabled, SetFullscreenStretchEnabled);
}

SettingsRow PauseInQuickMenuRow() {
    return BoolRow("Pause In Quick Menu", IsPauseInQuickMenu, SetPauseInQuickMenu);
}

SettingsRow MenuRotationRow() {
    return {"Menu Rotation", [] { return std::to_string(GetMenuRotation()) + " deg"; },
            [](int dir) { SetMenuRotation(std::clamp(GetMenuRotation() / 90 + dir, 0, 3) * 90); }};
}

SettingsRow MenuInputRotationRow() {
    return BoolRow("Rotate Menu Input", IsMenuInputRotated, SetMenuInputRotated);
}

SettingsRow MovieThrottleClockRow() {
    return {"Movie CPU Clock",
            [] { return std::to_string(GetMovieThrottleClockPercentage()) + "%"; },
            [](int dir) {
                SetMovieThrottleClockPercentage(GetMovieThrottleClockPercentage() + dir * 5);
            }};
}

// The running timers keep their own copy of the clock scale, so a change has to be pushed into
// core timing to take effect without a reboot.
SettingsRow CpuClockRow() {
    auto& setting = Settings::values.cpu_clock_percentage;
    return {"CPU Clock", [&setting] { return std::to_string(setting.GetValue()) + "%"; },
            [&setting](int dir) {
                setting = std::clamp(setting.GetValue() + dir * 5, 25, 400);
                auto& system = Core::System::GetInstance();
                if (system.IsPoweredOn()) {
                    system.CoreTiming().UpdateClockSpeed(static_cast<u32>(setting.GetValue()));
                }
            }};
}

// The DSP caches the stretcher's state, so writing the setting alone would not reach it.
SettingsRow AudioStretchingRow() {
    auto& setting = Settings::values.enable_audio_stretching;
    const auto get = [&setting] { return setting.GetValue(); };
    return {"Audio Stretching", [get] { return BoolText(get()); },
            [&setting](int dir) {
                setting = dir > 0;
                auto& system = Core::System::GetInstance();
                if (system.IsPoweredOn()) {
                    system.DSP().EnableStretching(dir > 0);
                }
            },
            SettingsModal::None, get};
}

SettingsRow PointerSourceRow() {
    return {"Touch Pointer Source",
            [] { return std::string{PointerSourceName(GetPointerSource())}; },
            [](int dir) {
                SetPointerSource(static_cast<PointerSource>(std::clamp(
                    static_cast<int>(GetPointerSource()) + dir, 0, NumPointerSources - 1)));
            }};
}

SettingsRow GyroSensitivityRow(const char* label, bool horizontal) {
    return {label,
            [horizontal] {
                return std::to_string(horizontal ? GetGyroSensitivityX() : GetGyroSensitivityY()) +
                       "%";
            },
            [horizontal](int dir) {
                const int x = GetGyroSensitivityX();
                const int y = GetGyroSensitivityY();
                const int stepped = std::clamp((horizontal ? x : y) + dir * 10, 10, 500);
                SetGyroSensitivity(horizontal ? stepped : x, horizontal ? y : stepped);
            }};
}

SettingsRow GyroSourceRow() {
    return {"Gyro Source", [] { return std::string{GyroSourceName(GetGyroSource())}; },
            [](int dir) {
                SetGyroSource(static_cast<GyroSource>(
                    std::clamp(static_cast<int>(GetGyroSource()) + dir, 0, NumGyroSources - 1)));
            }};
}

// The screen arrangement rows the quick menu shows. These go through the frontend's steppers
// rather than the settings directly, because those also queue the layout refresh.
SettingsRow ScreenLayoutRow() {
    return {"Screen Layout", [] { return std::string{CurrentScreenLayoutName()}; },
            [](int dir) { StepScreenLayout(dir); }};
}

SettingsRow SwapScreensRow() {
    return {"Swap Screens", [] { return std::string{}; }, [](int) { ToggleSwapScreens(); }};
}

SettingsRow ScreenGapRow() {
    return {"Screen Gap", [] { return std::to_string(GetScreenGap()) + " px"; },
            [](int dir) { StepScreenGap(dir); }};
}

SettingsRow OverlayPositionRow() {
    return {"Overlay Position", [] { return std::string{OverlayScreenPositionName()}; },
            [](int dir) { StepOverlayScreenPosition(dir); }};
}

SettingsRow OverlaySizeRow() {
    return {"Overlay Size", [] { return std::to_string(GetOverlayScreenSize()) + "%"; },
            [](int dir) { StepOverlayScreenSize(dir); }};
}

SettingsRow OverlayOpacityRow() {
    return {"Overlay Opacity", [] { return std::to_string(GetOverlayScreenOpacity()) + "%"; },
            [](int dir) { StepOverlayScreenOpacity(dir); }};
}

// Show size only when the page was built
SettingsRow ClearShaderCacheRow() {
    const std::uint64_t bytes = GetShaderCacheSize();
    return {"Clear Shader Cache", [bytes] { return FormatSize(bytes); }, {},
            SettingsModal::ClearShaderCache};
}

// "N of M" summary of how many layouts R3 is set to cycle through.
SettingsRow LayoutCycleRow() {
    return {"R3 Screen Layouts",
            [] {
                const int total = GetScreenLayoutCount();
                const std::uint32_t mask = GetLayoutCycleMask();
                int enabled = 0;
                for (int i = 0; i < total; ++i) {
                    enabled += (mask & (1u << i)) != 0 ? 1 : 0;
                }
                return std::to_string(enabled) + " of " + std::to_string(total);
            },
            {},
            SettingsModal::LayoutCycle};
}

SettingsRow UpdateChannelRow() {
    return {
        "Update Channel",
        [] { return std::string{GetUpdateChannel() == UpdateChannel::Stable ? "Stable" : "Prerelease"}; },
        [](int) {
            SetUpdateChannel(GetUpdateChannel() == UpdateChannel::Stable ? UpdateChannel::Prerelease
                                                                         : UpdateChannel::Stable);
        }};
}

} // namespace

const char* SettingsPageName(SettingsPage page) {
    switch (page) {
    case SettingsPage::General:
        return "General";
    case SettingsPage::System:
        return "System";
    case SettingsPage::Console:
        return "Console";
    case SettingsPage::Graphics:
        return "Graphics";
    case SettingsPage::Enhancements:
        return "Enhancements";
    case SettingsPage::Audio:
        return "Audio";
    case SettingsPage::Layout:
        return "Layout";
    case SettingsPage::Controls:
        return "Controls";
    case SettingsPage::Storage:
        return "Storage";
    case SettingsPage::Experimental:
        return "Experimental";
    case SettingsPage::Debug:
        return "Debug";
    default:
        return "";
    }
}

std::vector<SettingsRow> BuildSettingsPage(SettingsPage page) {
    auto& v = Settings::values;
    switch (page) {
    case SettingsPage::General:
        return {
            FrameLimitRow(),
            CpuClockRow(),
            Toggle("Show FPS Counter", v.show_fps),
            Toggle("Shader Compile Notice", v.show_shader_compile_notice),
            PauseInQuickMenuRow(),
            UpdateChannelRow(),
            BoolRow("Show What's New", IsWhatsNewCardEnabled, SetWhatsNewCardEnabled),
            {"Check for Updates",
             [] { return std::string{CurrentVersion()}; },
             {},
             SettingsModal::CheckForUpdates},
            {"Release Notes",
             [] { return std::string{CurrentVersion()}; },
             {},
             SettingsModal::ReleaseNotes},
            {"Reset All Settings",
             [] { return std::string{"Choose"}; },
             {},
             SettingsModal::ResetDefaults},
        };
    case SettingsPage::System:
        return {
            Toggle("New 3DS Mode", v.is_new_3ds),
            RegionRow(),
            UsernameRow(),
            BirthMonthRow(),
            BirthDayRow(),
            LanguageRow(),
            CountryRow(),
            SoundOutputRow(),
            PlayCoinsRow(),
            SystemSetupRow(),
            Toggle("LLE Applets", v.lle_applets),
            Toggle("Required Online LLE Modules", v.enable_required_online_lle_modules),
            Toggle("Region Free Patch", v.apply_region_free_patch),
            Choice("Clock Source", v.init_clock, kInitClockNames),
            FixedClockRow(),
            ClockOffsetRow(),
            Choice("Initial Ticks", v.init_ticks_type, kInitTicksNames),
            InitTicksValueRow(),
            Number("Pedometer Steps Per Hour", v.steps_per_hour, 0, 10000, 100),
            Toggle("Plugin Loader", v.plugin_loader_enabled),
            Toggle("Allow Games To Change Plugin Loader", v.allow_plugin_loader),
        };
    case SettingsPage::Console:
        return {
            ConsoleIdRow(),
            MacAddressRow(),
            UniqueDataRow(UniqueDataFile::SecureInfo),
            UniqueDataRow(UniqueDataFile::FriendCodeSeed),
            UniqueDataRow(UniqueDataFile::Otp),
            UniqueDataRow(UniqueDataFile::Movable),
            UnlinkConsoleRow(),
            Toggle("Unique Data Console Type", v.toggle_unique_data_console_type),
        };
    case SettingsPage::Graphics:
        return {
            GraphicsApiRow(),
            Toggle("Hardware Shader", v.use_hw_shader),
            Toggle("Accurate Shader Multiplication", v.shaders_accurate_mul),
            Toggle("Shader JIT", v.use_shader_jit),
            Toggle("Async Shader Compilation", v.async_shader_compilation),
            Toggle("Disk Shader Cache", v.use_disk_shader_cache),
            ClearShaderCacheRow(),
            Toggle("Async GPU (restart)", v.async_gpu_emulation),
            Toggle("Strict GPU Sync", v.strict_gpu_sync),
            Toggle("Async Presentation", v.async_presentation),
            Toggle("VSync", v.use_vsync),
            Toggle("Detect Display Refresh Rate", v.use_display_refresh_rate_detection),
            Toggle("Skip Duplicate Frames", v.use_skip_duplicate_frames),
            Toggle("SPIR-V Shader Generation", v.spirv_shader_gen),
            Toggle("Disable SPIR-V Optimizer", v.disable_spirv_optimizer),
            Choice("Texture Sampling", v.texture_sampling, kTextureSamplingNames),
            Toggle("Simulate 3DS GPU Timings", v.simulate_3ds_gpu_timings),
            Number("Render Thread Delay", v.delay_game_render_thread_us, 0, 10000, 100, " us"),
        };
    case SettingsPage::Enhancements:
        return {
            ResolutionRow(),
            Toggle("Integer Scaling", v.use_integer_scaling),
            Toggle("Linear Filtering", v.filter_mode),
            Choice("Texture Filter", v.texture_filter, kTextureFilterNames),
            Choice("Anisotropic Filtering", v.anisotropic_filtering, kAnisotropyNames),
            StereoModeRow(),
            StereoDepthRow(),
            Toggle("Swap Eyes", v.swap_eyes_3d),
            Relayout(Number("Labo VR Image Size", v.cardboard_screen_size, 30, 100, 5, "%")),
            Relayout(Number("Labo VR Horizontal Align", v.cardboard_x_shift, -100, 100, 5, "%")),
            Relayout(Number("Labo VR Vertical Align", v.cardboard_y_shift, -100, 100, 5, "%")),
            Choice("Eye Rendered In 2D", v.mono_render_option, kMonoEyeNames),
            DisableRightEyeRow(),
            Toggle("Custom Textures", v.custom_textures),
            Toggle("Preload Custom Textures", v.preload_textures),
            Toggle("Async Custom Texture Loading", v.async_custom_loading),
            Toggle("Dump Textures", v.dump_textures),
        };
    case SettingsPage::Audio:
        return {
            Choice("Audio Emulation", v.audio_emulation, kAudioEmulationNames),
            Percent("Volume", v.volume, 0, 100, 5),
            AudioStretchingRow(),
            Toggle("Realtime Audio", v.enable_realtime_audio),
            Toggle("Simulate Headphones", v.simulate_headphones_plugged),
        };
    case SettingsPage::Layout:
        return {
            MenuRotationRow(),
            MenuInputRotationRow(),
            StretchFullscreenRow(),
            Number("Screen Gap", v.screen_gap, 0, 200, 4, " px"),
            LargeScreenProportionRow(),
            Choice("Overlay Screen Position", v.overlay_screen_position, kOverlayPositionNames),
            Number("Overlay Screen Size", v.overlay_screen_size, 10, 60, 5, "%"),
            Number("Overlay Screen Opacity", v.overlay_screen_opacity, 10, 100, 10, "%"),
            Number("Top Screen Padding X", v.screen_top_leftright_padding, 0, 200, 4, " px"),
            Number("Top Screen Padding Y", v.screen_top_topbottom_padding, 0, 200, 4, " px"),
            Number("Bottom Screen Padding X", v.screen_bottom_leftright_padding, 0, 200, 4, " px"),
            Number("Bottom Screen Padding Y", v.screen_bottom_topbottom_padding, 0, 200, 4, " px"),
            ColorChannel("Background Red", v.bg_red),
            ColorChannel("Background Green", v.bg_green),
            ColorChannel("Background Blue", v.bg_blue),
            LayoutCycleRow(),
        };
    case SettingsPage::Controls:
        return {
            PointerSourceRow(),
            GyroSensitivityRow("Gyro Sensitivity X", true),
            GyroSensitivityRow("Gyro Sensitivity Y", false),
            GyroSourceRow(),
            Toggle("Use Artic Base Controller", v.use_artic_base_controller),
            {"Controller Mapping",
             [] { return std::string{"Configure"}; },
             {},
             SettingsModal::ControllerMap},
        };
    case SettingsPage::Storage:
        return {
            Toggle("Virtual SD Card", v.use_virtual_sd),
            Toggle("Compress CIA Installs", v.compress_cia_installs),
            Toggle("Async Filesystem Operations", v.async_fs_operations),
        };
    case SettingsPage::Experimental:
        return {
            BoolRow("Movie CPU Throttle", IsMovieThrottleEnabled, SetMovieThrottleEnabled),
            MovieThrottleClockRow(),
            Toggle("Skip Slow Draw", v.skip_slow_draw),
            Toggle("Skip Texture Copy", v.skip_texture_copy),
            Toggle("Skip CPU Write", v.skip_cpu_write),
        };
    case SettingsPage::Debug:
        return {
            Toggle("CPU JIT", v.use_cpu_jit),
            Toggle("Fastmem (restart)", v.fastmem),
            Toggle("Deterministic Async Operations", v.deterministic_async_operations),
            Toggle("Delay Start For LLE Modules", v.delay_start_for_lle_modules),
            Toggle("Break On Unmapped Memory", v.break_on_unmapped_memory_access),
            Toggle("Renderer Debug", v.renderer_debug),
            Toggle("Dump Command Buffers", v.dump_command_buffers),
            Toggle("Instant Debug Log", v.instant_debug_log),
            {"Log Filter",
             [] { return Settings::values.log_filter.GetValue(); },
             {},
             SettingsModal::LogFilter},
        };
    default:
        return {};
    }
}

const char* QuickPageName(QuickPage page) {
    switch (page) {
    case QuickPage::Display:
        return "Display";
    case QuickPage::Graphics:
        return "Graphics";
    case QuickPage::Stereo:
        return "3D";
    case QuickPage::Audio:
        return "Audio";
    case QuickPage::Input:
        return "Input";
    default:
        return "System";
    }
}

std::vector<SettingsRow> BuildQuickPage(QuickPage page) {
    auto& v = Settings::values;
    switch (page) {
    case QuickPage::Display: {
        std::vector<SettingsRow> rows{
            ScreenLayoutRow(),
            SwapScreensRow(),
            ScreenGapRow(),
            Relayout(LargeScreenProportionRow()),
        };
        if (IsOverlayScreenLayout()) {
            rows.push_back(OverlayPositionRow());
            rows.push_back(OverlaySizeRow());
            rows.push_back(OverlayOpacityRow());
        }
        rows.push_back(StretchFullscreenRow());
        rows.push_back(MenuRotationRow());
        rows.push_back(MenuInputRotationRow());
        rows.push_back(Toggle("FPS Counter", v.show_fps));
        rows.push_back(Toggle("Shader Compile Notice", v.show_shader_compile_notice));
        return rows;
    }
    case QuickPage::Graphics:
        return {
            ResolutionRow(),
            Relayout(Toggle("Integer Scaling", v.use_integer_scaling)),
            Toggle("Linear Filtering", v.filter_mode),
            Choice("Texture Filter", v.texture_filter, kTextureFilterNames),
            Choice("Texture Sampling", v.texture_sampling, kTextureSamplingNames),
            Choice("Anisotropic Filtering", v.anisotropic_filtering, kAnisotropyNames),
            Toggle("Hardware Shader", v.use_hw_shader),
            Toggle("Custom Textures", v.custom_textures),
        };
    case QuickPage::Stereo:
        return {
            StereoModeRow(),
            StereoDepthRow(),
            Toggle("Swap Eyes", v.swap_eyes_3d),
            Relayout(Number("Labo VR Image Size", v.cardboard_screen_size, 30, 100, 5, "%")),
            Relayout(Number("Labo VR Horizontal Align", v.cardboard_x_shift, -100, 100, 5, "%")),
            Relayout(Number("Labo VR Vertical Align", v.cardboard_y_shift, -100, 100, 5, "%")),
            Choice("Eye Rendered In 2D", v.mono_render_option, kMonoEyeNames),
            DisableRightEyeRow(),
        };
    case QuickPage::Audio:
        return {
            Percent("Volume", v.volume, 0, 100, 5),
            AudioStretchingRow(),
            Toggle("Simulate Headphones", v.simulate_headphones_plugged),
        };
    case QuickPage::Input:
        return {
            PointerSourceRow(),
            BoolRow("Pointer Mode", IsPointerModeActive, SetPointerMode),
            GyroSensitivityRow("Gyro Sensitivity X", true),
            GyroSensitivityRow("Gyro Sensitivity Y", false),
            GyroSourceRow(),
        };
    default:
        return {
            CpuClockRow(),
            FrameLimitRow(),
            PauseInQuickMenuRow(),
        };
    }
}

const char* ActiveGraphicsBackendName() {
    return GraphicsApiName(Settings::GetWorkingGraphicsAPI());
}

std::string GetLogFilter() {
    return Settings::values.log_filter.GetValue();
}

void SetLogFilter(const std::string& filter) {
    Settings::values.log_filter = filter;
    Common::Log::Filter log_filter;
    log_filter.ParseFilterString(filter);
    Common::Log::SetGlobalFilter(log_filter);
}

std::string GetProfileUsername() {
    return Common::UTF16ToUTF8(GetProfile().username);
}

void SetProfileUsername(const std::string& name) {
    std::u16string wide = Common::UTF8ToUTF16(name);
    if (wide.size() > 10) {
        wide.resize(10);
    }
    GetProfile().username = wide;
}

const std::vector<CountryOption>& CountryOptions() {
    static const std::vector<CountryOption> options = [] {
        std::vector<CountryOption> list;
        for (std::size_t i = 0; i < kCountryNames.size(); ++i) {
            if (kCountryNames[i][0] != '\0') {
                list.push_back({static_cast<int>(i), kCountryNames[i]});
            }
        }
        return list;
    }();
    return options;
}

int GetProfileCountry() {
    return GetProfile().country;
}

void SetProfileCountry(int code) {
    GetProfile().country = code;
}

bool IsCountryValidForRegion(int code) {
    const s32 region = Settings::values.region_value.GetValue();
    if (region == Settings::REGION_VALUE_AUTO_SELECT) {
        return true;
    }
    return Service::CFG::Module::IsValidRegionCountry(static_cast<u32>(region),
                                                      static_cast<u8>(code));
}

std::string GetFixedClockText() {
    const auto time = static_cast<std::time_t>(Settings::values.init_time.GetValue());
    std::tm tm{};
    if (gmtime_r(&time, &tm) == nullptr) {
        return "2000-01-01 00:00:00";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buffer;
}

bool SetFixedClockText(const std::string& text) {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d:%d", &year, &month, &day, &hour, &minute,
                    &second) != 6) {
        return false;
    }
    if (year < 2000 || year > 9999 || month < 1 || month > 12 || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) {
        return false;
    }
    if (day < 1 || day > DaysInMonth(year, month)) {
        return false;
    }
    Settings::values.init_time =
        static_cast<u64>(DaysFromCivil(year, month, day)) * 86400 +
        static_cast<u64>(hour) * 3600 + static_cast<u64>(minute) * 60 + static_cast<u64>(second);
    return true;
}

std::string GetInitTicksText() {
    return std::to_string(Settings::values.init_ticks_override.GetValue());
}

void SetInitTicksText(const std::string& text) {
    try {
        Settings::values.init_ticks_override = static_cast<s64>(std::stoll(text));
    } catch (const std::exception&) {
        // Leave the old value in place on anything unparsable.
    }
}

std::string GetConsoleIdText() {
    char buffer[24];
    std::snprintf(buffer, sizeof(buffer), "0x%016llX",
                  static_cast<unsigned long long>(Cfg().GetConsoleUniqueId()));
    return buffer;
}

std::string GetMacAddressText() {
    return Cfg().GetMacAddress();
}

void RegenerateConsoleId() {
    auto& cfg = Cfg();
    const auto [random_number, console_id] = cfg.GenerateConsoleUniqueId();
    cfg.SetConsoleUniqueId(random_number, console_id);
    cfg.UpdateConfigNANDSavegame();
}

void RegenerateMacAddress() {
    auto& cfg = Cfg();
    cfg.GetMacAddress() = Service::CFG::GenerateRandomMAC();
    cfg.SaveMacAddress();
}

const char* UniqueDataFileName(UniqueDataFile file) {
    switch (file) {
    case UniqueDataFile::SecureInfo:
        return "SecureInfo_A";
    case UniqueDataFile::FriendCodeSeed:
        return "LocalFriendCodeSeed_B";
    case UniqueDataFile::Otp:
        return "OTP";
    case UniqueDataFile::Movable:
        return "movable.sed";
    default:
        return "";
    }
}

std::string UniqueDataStatus(UniqueDataFile file) {
    switch (LoadUniqueData(file)) {
    case HW::UniqueData::SecureDataLoadStatus::Loaded:
        return "Loaded";
    case HW::UniqueData::SecureDataLoadStatus::InvalidSignature:
        return "Invalid signature";
    case HW::UniqueData::SecureDataLoadStatus::RegionChanged:
        return "Loaded, region changed";
    case HW::UniqueData::SecureDataLoadStatus::CannotValidateSignature:
        return "Loaded, unverified";
    case HW::UniqueData::SecureDataLoadStatus::NotFound:
        return "Not found";
    case HW::UniqueData::SecureDataLoadStatus::Invalid:
        return "Invalid";
    case HW::UniqueData::SecureDataLoadStatus::IOError:
        return "Read error";
    case HW::UniqueData::SecureDataLoadStatus::NoCryptoKeys:
        return "Missing keys";
    default:
        return "Unknown";
    }
}

bool InstallUniqueDataFile(UniqueDataFile file, const std::string& from) {
    const std::string source =
        FileUtil::SanitizePath(from, FileUtil::DirectorySeparator::PlatformDefault);
    const std::string destination =
        FileUtil::SanitizePath(UniqueDataPath(file), FileUtil::DirectorySeparator::PlatformDefault);
    if (source.empty() || destination.empty() || source == destination) {
        return false;
    }
    FileUtil::CreateFullPath(destination);
    if (!FileUtil::Copy(source, destination)) {
        return false;
    }
    HW::UniqueData::InvalidateSecureData();
    return true;
}

bool IsConsoleLinked() {
    return HW::UniqueData::IsFullConsoleLinked();
}

void UnlinkConsole() {
    HW::UniqueData::UnlinkConsole();
}

void RefreshSystemSettings() {
    s_cfg.reset();
    s_profile_loaded = false;
    s_play_coins.reset();
}

void CommitSettings() {
    SaveConfig();

    if (s_play_coins && *s_play_coins != Service::PTM::Module::GetPlayCoins()) {
        Service::PTM::Module::SetPlayCoins(static_cast<u16>(*s_play_coins));
    }

    if (s_profile_loaded) {
        auto& cfg = Cfg();
        bool modified = false;
        const Profile& now = s_profile;
        const Profile& before = s_profile_saved;
        if (now.username != before.username) {
            cfg.SetUsername(now.username);
            modified = true;
        }
        if (now.birth_month != before.birth_month || now.birth_day != before.birth_day) {
            cfg.SetBirthday(static_cast<u8>(now.birth_month), static_cast<u8>(now.birth_day));
            modified = true;
        }
        if (now.language != before.language) {
            cfg.SetSystemLanguage(static_cast<Service::CFG::SystemLanguage>(now.language));
            modified = true;
        }
        if (now.sound_mode != before.sound_mode) {
            cfg.SetSoundOutputMode(static_cast<Service::CFG::SoundOutputMode>(now.sound_mode));
            modified = true;
        }
        // SetCountryCode also resets the state code, so it is only written when it changed.
        if (now.country != before.country) {
            cfg.SetCountryCode(static_cast<u8>(now.country));
            modified = true;
        }
        if (now.system_setup != before.system_setup) {
            cfg.SetSystemSetupNeeded(now.system_setup);
            modified = true;
        }
        if (modified) {
            cfg.UpdateConfigNANDSavegame();
        }
    }

    RefreshSystemSettings();
}

} // namespace SwitchFrontend
