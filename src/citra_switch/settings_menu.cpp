// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string>

#include "citra_switch/config.h"
#include "citra_switch/settings_menu.h"
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "core/core.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/ptm/ptm.h"
#include "core/hw/unique_data.h"

namespace SwitchFrontend {

namespace {

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

} // namespace

namespace {

std::vector<SettingsRow> RowsFor(const std::vector<const SettingEntry*>& entries,
                                 bool with_headings) {
    std::vector<SettingsRow> rows;
    rows.reserve(entries.size() + 8);
    const char* open_heading = nullptr;
    for (const SettingEntry* entry : entries) {
        if (with_headings && entry->subgroup != nullptr && entry->subgroup[0] != '\0' &&
            (open_heading == nullptr || std::strcmp(open_heading, entry->subgroup) != 0)) {
            open_heading = entry->subgroup;
            SettingsRow heading;
            heading.label = entry->subgroup;
            heading.is_header = true;
            rows.push_back(std::move(heading));
        }
        SettingsRow row;
        row.label = entry->label;
        row.value = entry->value;
        row.step = entry->step;
        row.modal = entry->modal;
        row.boolean = entry->boolean;
        row.description = entry->description != nullptr ? entry->description : "";
        row.needs_restart = (entry->flags & EntryFlag::Restart) != 0;
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

std::vector<SettingsRow> BuildCategoryRows(Category category) {
    return RowsFor(EntriesIn(category), true);
}

std::vector<SettingsRow> BuildQuickRows(QuickSection section) {
    return RowsFor(EntriesInQuick(section), false);
}

std::vector<SettingsRow> BuildSearchRows(const std::string& query) {
    const std::vector<const SettingEntry*> matches = SearchEntries(query);
    std::vector<SettingsRow> rows = RowsFor(matches, false);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Category category = matches[i]->category;
        const char* page = category == Category::Count ? "Quick Menu" : CategoryName(category);
        rows[i].label = std::string{page} + " > " + rows[i].label;
    }
    return rows;
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

int GetProfileValue(ProfileValue field) {
    const Profile& profile = GetProfile();
    switch (field) {
    case ProfileValue::BirthMonth:
        return profile.birth_month;
    case ProfileValue::BirthDay:
        return profile.birth_day;
    case ProfileValue::Language:
        return profile.language;
    default:
        return profile.sound_mode;
    }
}

void SetProfileValue(ProfileValue field, int value) {
    Profile& profile = GetProfile();
    switch (field) {
    case ProfileValue::BirthMonth:
        profile.birth_month = value;
        break;
    case ProfileValue::BirthDay:
        profile.birth_day = value;
        break;
    case ProfileValue::Language:
        profile.language = value;
        break;
    default:
        profile.sound_mode = value;
        break;
    }
}

int ProfileBirthMonthLength() {
    const int month = std::clamp(GetProfile().birth_month, 1, 12);
    return kDaysInMonth[static_cast<std::size_t>(month - 1)];
}

bool IsSystemSetupNeeded() {
    return GetProfile().system_setup;
}

void SetSystemSetupNeeded(bool needed) {
    GetProfile().system_setup = needed;
}

int GetPlayCoins() {
    return ReadPlayCoins();
}

void SetPlayCoins(int coins) {
    s_play_coins = coins;
}

const char* ProfileCountryName() {
    const auto index = static_cast<std::size_t>(GetProfile().country);
    if (index >= kCountryNames.size() || kCountryNames[index][0] == '\0') {
        return "Unknown";
    }
    return kCountryNames[index];
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
