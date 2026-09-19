// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>
#include <fmt/format.h>
#include <INIReader.h>
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "citra_switch/camera/still_image_camera.h"
#include "citra_switch/config.h"
#include "citra_switch/settings_registry.h"
#include "citra_switch/input.h"
#include "citra_switch/overlay_menu.h"
#include "core/frontend/camera/factory.h"
#include "core/hle/service/cam/cam_params.h"
#include "core/hle/service/service.h"
#include "core/system_titles.h"
#include "video_core/overlay.h"

namespace {

constexpr const char* kDefaultUserDir = "sdmc:/switch/dekopon/";

constexpr const char* kUserDirPointer = "sdmc:/switch/dekopon/user_dir.txt";

std::string WithTrailingSlash(std::string path) {
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    return path;
}

// Reads the pointer file or returns "" to use default.
std::string ReadUserDirPointer() {
    std::string contents;
    FileUtil::ReadFileToString(true, kUserDirPointer, contents);
    std::string path = WithTrailingSlash(Common::StripSpaces(contents));
    if (path == kDefaultUserDir) {
        return "";
    }
    return path;
}

void WriteUserDirPointer(const std::string& user_dir) {
    // Removing the stub rather than writing the default back keeps a default install clean.
    if (user_dir.empty() || user_dir == kDefaultUserDir) {
        FileUtil::Delete(kUserDirPointer);
        return;
    }
    FileUtil::CreateFullPath(kUserDirPointer);
    FileUtil::WriteStringToFile(true, kUserDirPointer, user_dir);
}

SwitchFrontend::SwitchPaths s_paths;
std::string s_active_user_dir;
std::string s_inserted_cartridge;
std::string s_artic_base_address;
std::string s_camera_image;
SwitchFrontend::CameraTarget s_camera_target = SwitchFrontend::CameraTarget::All;
int s_menu_rotation = 0;
bool s_menu_input_rotated = false;
SwitchFrontend::UpdateChannel s_update_channel = SwitchFrontend::UpdateChannel::Stable;
std::string s_dismissed_update_tag;
std::string s_last_seen_version;
bool s_whats_new_card = true;

// Pushes the picked image onto the three camera slots the CAM service reads.
void ApplyCameraSettings() {
    namespace CAM = Service::CAM;

    const bool inner = s_camera_target != SwitchFrontend::CameraTarget::Outer;
    const bool outer = s_camera_target != SwitchFrontend::CameraTarget::Inner;
    const std::array<bool, CAM::NumCameras> fed{outer, inner, outer};

    for (int i = 0; i < CAM::NumCameras; ++i) {
        const bool use_image = !s_camera_image.empty() && fed[static_cast<std::size_t>(i)];
        Settings::values.camera_name[i] = use_image ? "image" : "blank";
        Settings::values.camera_config[i] = use_image ? s_camera_image : std::string{};
        // The inner camera faces the player, so its picture comes back mirrored on hardware.
        Settings::values.camera_flip[i] =
            use_image && i == CAM::InnerCamera ? static_cast<int>(CAM::Flip::Horizontal) : 0;
    }
}

// INIReader has no "is this key present" call for some reason.
constexpr const char* kAbsentValue = "\x01" "dekopon-absent";

std::uint64_t s_per_game_id = 0;

std::string PerGameConfigPath(std::uint64_t program_id) {
    return fmt::format("{}custom/{:016X}.ini",
                       FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir), program_id);
}

// Reads/Writes the SD-card config file
class Config {
public:
    Config() {
        config_loc = FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "config.ini";
        std::string ini_buffer;
        FileUtil::ReadFileToString(true, config_loc, ini_buffer);
        if (!ini_buffer.empty()) {
            config = std::make_unique<INIReader>(ini_buffer.c_str(), ini_buffer.size());
        }
        Reload();
    }

    void Reload() {
        if (config == nullptr || config->ParseError() < 0) {
            if (config != nullptr) {
                LOG_WARNING(Config, "Failed to parse {}, falling back to defaults", config_loc);
            }
            config = std::make_unique<INIReader>("", 0);
        } else {
            LOG_INFO(Config, "Successfully loaded {}", config_loc);
        }
        ReadValues();
    }

    void Save() {
        FileUtil::CreateFullPath(config_loc);
        FileUtil::WriteStringToFile(true, config_loc, BuildINI());
        LOG_INFO(Config, "Saved config to {}", config_loc);
    }

    int LaunchCount() const {
        return launch_count;
    }

    void ResetToDefaults() {
        SwitchFrontend::ResetEntriesToDefaults();
        ApplyDefaultMappings();
        ApplyCameraSettings();
    }

private:
    std::unique_ptr<INIReader> config;
    std::string config_loc;
    int launch_count = 0;

    void ReadRegistry() {
        for (const SwitchFrontend::SettingEntry& entry : SwitchFrontend::Registry()) {
            if (!entry.IsPersisted()) {
                continue;
            }
            entry.load(entry.default_text());
            const std::string raw = config->Get(entry.group, entry.id, kAbsentValue);
            if (raw != kAbsentValue) {
                entry.load(raw);
            }
        }
    }

    void ApplyDefaultMappings() {
        for (int i = 0; i < SwitchFrontend::NumMappableControls; ++i) {
            const auto control = static_cast<SwitchFrontend::MappableControl>(i);
            SwitchFrontend::SetMapping(control, SwitchFrontend::DefaultMapping(control));
        }
        SwitchFrontend::ApplyButtonMappings();
    }

    void ReadValues() {
        ReadRegistry();

        if (Settings::values.render_3d.GetValue() == Settings::StereoRenderOption::Off) {
            Settings::values.factor_3d = 0;
        }

        if (config->Get("Switch", "stretch_fullscreen", kAbsentValue) == kAbsentValue) {
            SwitchFrontend::SetFullscreenStretchEnabled(
                config->GetBoolean("Layout", "stretch_fullscreen", false));
        }

        // The core expects every known service module to have an explicit setting and crashes if not.
        for (const auto& service_module : Service::service_module_map) {
            Settings::values.lle_modules.emplace(service_module.name, false);
        }

        s_paths.roms_dir =
            WithTrailingSlash(Common::StripSpaces(config->Get("Switch", "roms_dir", "")));
        if (s_paths.roms_dir.empty()) {
            s_paths.roms_dir = SwitchFrontend::GetDefaultRomsDir(s_paths.user_dir);
        }
        s_paths.roms_dir_2 =
            WithTrailingSlash(Common::StripSpaces(config->Get("Switch", "roms_dir_2", "")));
        s_paths.scan_recursive = config->GetBoolean("Switch", "scan_recursive", true);
        s_inserted_cartridge =
            Common::StripSpaces(config->Get("Switch", "inserted_cartridge", ""));
        if (!s_inserted_cartridge.empty() && !FileUtil::Exists(s_inserted_cartridge)) {
            s_inserted_cartridge.clear();
        }
        s_artic_base_address =
            Common::StripSpaces(config->Get("Switch", "last_artic_base_addr", ""));

        s_camera_image = Common::StripSpaces(config->Get("Camera", "image", ""));
        if (!s_camera_image.empty() && !FileUtil::Exists(s_camera_image)) {
            s_camera_image.clear();
        }
        s_camera_target = static_cast<SwitchFrontend::CameraTarget>(
            std::clamp<long>(config->GetInteger("Camera", "target", 0), 0,
                             SwitchFrontend::NumCameraTargets - 1));
        ApplyCameraSettings();

        s_dismissed_update_tag =
            Common::StripSpaces(config->Get("Switch", "dismissed_update", ""));
        s_last_seen_version =
            Common::StripSpaces(config->Get("Switch", "last_seen_version", ""));

        // Each control stores the index of the physical Switch button it drives.
        for (int i = 0; i < SwitchFrontend::NumMappableControls; ++i) {
            const auto control = static_cast<SwitchFrontend::MappableControl>(i);
            const int def = static_cast<int>(SwitchFrontend::DefaultMapping(control));
            const int raw =
                config->GetInteger("Controls", SwitchFrontend::ControlConfigKey(control), def);
            const int clamped = std::clamp(raw, 0, SwitchFrontend::NumBindingChoices - 1);
            SwitchFrontend::SetMapping(control,
                                       static_cast<SwitchFrontend::InputButton>(clamped));
        }
        SwitchFrontend::ApplyButtonMappings();

        launch_count = config->GetInteger("Switch", "launch_count", 0) + 1;
    }

    std::string BuildINI() const {
        std::vector<std::string> order;
        std::map<std::string, std::string> bodies;

        const auto section = [&](const std::string& name) -> std::string& {
            if (bodies.find(name) == bodies.end()) {
                order.push_back(name);
            }
            return bodies[name];
        };

        for (const SwitchFrontend::SettingEntry& entry : SwitchFrontend::Registry()) {
            if (!entry.IsPersisted()) {
                continue;
            }
            std::string& out = section(entry.group);
            if (entry.description != nullptr && entry.description[0] != '\0') {
                out += "# ";
                out += entry.description;
                out += '\n';
            }
            out += entry.id;
            out += " = ";
            out += entry.save_global ? entry.save_global() : entry.save();
            out += '\n';
        }

        // The rest is state rather than settings, so none of it appears in the menus.
        {
            std::string& out = section("Switch");
            out += "# Directory scanned for titles.\n";
            out += "roms_dir = " + s_paths.roms_dir + '\n';
            out += "# Optional second directory scanned for titles.\n";
            out += "roms_dir_2 = " + s_paths.roms_dir_2 + '\n';
            out += "# Descend into the ROM directories' subfolders when scanning.\n";
            out += std::string{"scan_recursive = "} + (s_paths.scan_recursive ? "true" : "false") +
                   '\n';
            out += "# Cartridge image offered to software launched from the HOME Menu.\n";
            out += "inserted_cartridge = " + s_inserted_cartridge + '\n';
            out += "# Last address used for Artic Base or the Artic Setup Tool.\n";
            out += "last_artic_base_addr = " + s_artic_base_address + '\n';
            out += "# Release tag declined at the startup prompt. Manual checks ignore it.\n";
            out += "dismissed_update = " + s_dismissed_update_tag + '\n';
            out += "# Build that last reached the launcher.\n";
            out += "last_seen_version = " + s_last_seen_version + '\n';
            out += "launch_count = " + std::to_string(launch_count) + '\n';
        }
        {
            std::string& out = section("Camera");
            out += "# PNG or JPEG the emulated cameras show. Anything in camera/ is offered.\n";
            out += "image = " + s_camera_image + '\n';
            out += "# Which cameras the image feeds. 0: all, 1: outer only, 2: inner only.\n";
            out += "target = " + std::to_string(static_cast<int>(s_camera_target)) + '\n';
        }
        {
            std::string& out = section("Controls");
            out += "# Controller remapping, editable from Settings > Controls.\n";
            out += "# Controls are stored by index, like so:\n";
            out += "#   0:A 1:B 2:X 3:Y 4:Up 5:Down 6:Left 7:Right 8:L 9:R 10:+ 11:- 12:ZL 13:ZR\n";
            out += "#   14:L3 15:R3 16 leaves the control unbound.\n";
            for (int i = 0; i < SwitchFrontend::NumMappableControls; ++i) {
                const auto control = static_cast<SwitchFrontend::MappableControl>(i);
                out += std::string{SwitchFrontend::ControlConfigKey(control)} + " = " +
                       std::to_string(static_cast<int>(SwitchFrontend::GetMapping(control))) + '\n';
            }
        }

        std::ostringstream ss;
        for (const std::string& name : order) {
            if (ss.tellp() != 0) {
                ss << '\n';
            }
            ss << '[' << name << "]\n" << bodies[name];
        }
        return ss.str();
    }
};

// Kept alive past Bootstrap() so the menu can re-save settings while preserving launch_count.
std::unique_ptr<Config> s_config;

// Layered on top of a defaults reset, so each preset only lists what it changes.
void ApplyPreset(SwitchFrontend::SettingsPreset preset) {
    using SwitchFrontend::SettingsPreset;
    auto& v = Settings::values;

    if (preset == SettingsPreset::Default) {
        return;
    }

    v.async_gpu_emulation = true;
    v.async_shader_compilation = true;
    v.shaders_accurate_mul = false;
    v.disable_right_eye_render = true;
    SwitchFrontend::SetMovieThrottleEnabled(true);
    if (preset == SettingsPreset::Performance) {
        return;
    }

    // The arena stays off outside Ultra while it is being investigated.
    v.fastmem = true;
    v.anisotropic_filtering = Settings::AnisotropicFiltering::Off;
    v.skip_slow_draw = true;
    v.skip_texture_copy = true;
    v.skip_cpu_write = true;
}

} // namespace

namespace SwitchFrontend {

int Bootstrap() {
    // Resolve the dekopon directory and create its standard subdirectories.
    FileUtil::SetUserPath(ReadUserDirPointer());
    s_active_user_dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
    s_paths.user_dir = s_active_user_dir;
    FileUtil::CreateFullPath(s_active_user_dir + "amiibo/");
    FileUtil::CreateFullPath(s_active_user_dir + "camera/");

    Common::Log::Initialize();
    Common::Log::Start();

    // "image" is the factory name ApplyCameraSettings writes into the camera slots.
    Camera::RegisterFactory("image", std::make_unique<Camera::StillImage::Factory>());

    s_config = std::make_unique<Config>();

    // Apply the log filter the config just loaded.
    Common::Log::Filter log_filter;
    log_filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(log_filter);

    // Persist the bumped launch count and any defaulted settings for next time.
    s_config->Save();

    LOG_INFO(Frontend, "Dekopon launch #{}", s_config->LaunchCount());
    LOG_INFO(Frontend, "User directory: {}", s_active_user_dir);
    LOG_INFO(Frontend, "ROM directory: {} (recursive: {})", s_paths.roms_dir,
             s_paths.scan_recursive);
    if (!s_paths.roms_dir_2.empty()) {
        LOG_INFO(Frontend, "Second ROM directory: {}", s_paths.roms_dir_2);
    }
    LOG_INFO(Frontend, "Logging to: {}", FileUtil::GetUserPath(FileUtil::UserPath::LogDir));

    return s_config->LaunchCount();
}

int GetLaunchCount() {
    return s_config ? s_config->LaunchCount() : 0;
}

const SwitchPaths& GetPaths() {
    return s_paths;
}

void SetPaths(const SwitchPaths& paths) {
    s_paths.roms_dir = WithTrailingSlash(paths.roms_dir);
    s_paths.roms_dir_2 = WithTrailingSlash(Common::StripSpaces(paths.roms_dir_2));
    s_paths.scan_recursive = paths.scan_recursive;

    const std::string user_dir = WithTrailingSlash(paths.user_dir);
    if (user_dir != s_paths.user_dir) {
        s_paths.user_dir = user_dir;
        WriteUserDirPointer(user_dir);
        LOG_INFO(Frontend, "Dekopon directory set to {}, applies on the next launch", user_dir);
    }
    SaveConfig();
}

const std::string& GetInsertedCartridge() {
    return s_inserted_cartridge;
}

void SetInsertedCartridge(const std::string& path) {
    s_inserted_cartridge = path;
    SaveConfig();
}

const std::string& GetArticBaseAddress() {
    return s_artic_base_address;
}

void SetArticBaseAddress(const std::string& address) {
    s_artic_base_address = Common::StripSpaces(address);
    SaveConfig();
}

SystemFileSetupState GetSystemFileSetupState() {
    const auto [old3ds, new3ds] = Core::AreSystemTitlesInstalled();
    return {.old3ds = old3ds, .new3ds = new3ds};
}

void PrepareSystemFileSetup(SystemFileSetupMode mode) {
    Core::UninstallSystemFiles(mode == SystemFileSetupMode::Old3ds
                                   ? Core::SystemTitleSet::Old3ds
                                   : Core::SystemTitleSet::New3ds);
}

int GetMenuRotation() {
    return s_menu_rotation;
}

void SetMenuRotation(int degrees) {
    s_menu_rotation = std::clamp(degrees, 0, 359) / 90 * 90;
    VideoCore::SetOverlayRotation(static_cast<u32>(s_menu_rotation));
}

bool IsMenuInputRotated() {
    return s_menu_input_rotated;
}

void SetMenuInputRotated(bool enabled) {
    s_menu_input_rotated = enabled;
}

UpdateChannel GetUpdateChannel() {
    return s_update_channel;
}

void SetUpdateChannel(UpdateChannel channel) {
    s_update_channel = channel;
}

const std::string& GetDismissedUpdateTag() {
    return s_dismissed_update_tag;
}

void DismissUpdateTag(const std::string& tag) {
    s_dismissed_update_tag = tag;
    SaveConfig();
}

const std::string& GetLastSeenVersion() {
    return s_last_seen_version;
}

void RecordSeenVersion(const std::string& version) {
    if (s_last_seen_version == version) {
        return;
    }
    s_last_seen_version = version;
    SaveConfig();
}

bool IsWhatsNewCardEnabled() {
    return s_whats_new_card;
}

void SetWhatsNewCardEnabled(bool enabled) {
    s_whats_new_card = enabled;
}

MenuDirections RotateMenuDirections(MenuDirections pressed) {
    if (!s_menu_input_rotated) {
        return pressed;
    }
    switch (s_menu_rotation) {
    case 90:
        return {pressed.right, pressed.left, pressed.up, pressed.down};
    case 180:
        return {pressed.down, pressed.up, pressed.right, pressed.left};
    case 270:
        return {pressed.left, pressed.right, pressed.down, pressed.up};
    default:
        return pressed;
    }
}

bool GetUseArticBaseController() {
    return Settings::values.use_artic_base_controller.GetValue();
}

void SetUseArticBaseController(bool enabled) {
    Settings::values.use_artic_base_controller = enabled;
    SaveConfig();
}

const char* CameraTargetName(CameraTarget target) {
    switch (target) {
    case CameraTarget::Outer:
        return "Outer only";
    case CameraTarget::Inner:
        return "Inner only";
    default:
        return "All cameras";
    }
}

const std::string& GetCameraImage() {
    return s_camera_image;
}

CameraTarget GetCameraTarget() {
    return s_camera_target;
}

void SetCameraImage(const std::string& path, CameraTarget target) {
    s_camera_image = path;
    s_camera_target = target;
    ApplyCameraSettings();
    SaveConfig();
}

const std::string& GetActiveUserDir() {
    return s_active_user_dir;
}

std::string GetDefaultUserDir() {
    return kDefaultUserDir;
}

std::string GetDefaultRomsDir(const std::string& user_dir) {
    return WithTrailingSlash(user_dir.empty() ? kDefaultUserDir : user_dir) + "roms/";
}

void SaveConfig() {
    if (s_config) {
        s_config->Save();
    }
    SavePerGameConfig();
}

void ApplyPerGameConfig(std::uint64_t program_id) {
    ClearPerGameConfig();
    if (program_id == 0) {
        return;
    }
    s_per_game_id = program_id;

    std::string ini_buffer;
    FileUtil::ReadFileToString(true, PerGameConfigPath(program_id), ini_buffer);
    if (ini_buffer.empty()) {
        return;
    }
    INIReader ini{ini_buffer.c_str(), ini_buffer.size()};
    if (ini.ParseError() < 0) {
        LOG_WARNING(Config, "Failed to parse {}",
                    PerGameConfigPath(program_id));
        return;
    }

    int applied = 0;
    for (const SwitchFrontend::SettingEntry& entry : SwitchFrontend::Registry()) {
        if (!entry.IsOverridable()) {
            continue;
        }
        const std::string raw = ini.Get(entry.group, entry.id, kAbsentValue);
        if (raw == kAbsentValue) {
            continue;
        }
        entry.set_global(false);
        entry.load(raw);
        ++applied;
    }
    LOG_INFO(Config, "Applied {} setting override(s) for title {:016X}", applied, program_id);
}

void SavePerGameConfig() {
    if (s_per_game_id == 0) {
        return;
    }
    const std::string path = PerGameConfigPath(s_per_game_id);

    std::vector<std::string> order;
    std::map<std::string, std::string> bodies;
    int overrides = 0;
    for (const SwitchFrontend::SettingEntry& entry : SwitchFrontend::Registry()) {
        if (!entry.IsOverridden()) {
            continue;
        }
        if (bodies.find(entry.group) == bodies.end()) {
            order.emplace_back(entry.group);
        }
        bodies[entry.group] += std::string{entry.id} + " = " + entry.save() + '\n';
        ++overrides;
    }

    if (order.empty()) {
        if (FileUtil::Exists(path)) {
            FileUtil::Delete(path);
            LOG_INFO(Config, "Removed the settings file for title {:016X}", s_per_game_id);
        }
        return;
    }

    std::ostringstream ss;
    ss << "# Settings this title overrides. Everything else follows config.ini.\n";
    for (const std::string& name : order) {
        ss << '\n' << '[' << name << "]\n" << bodies[name];
    }
    FileUtil::CreateFullPath(path);
    FileUtil::WriteStringToFile(true, path, ss.str());
    LOG_INFO(Config, "Saved {} override(s) for title {:016X}", overrides, s_per_game_id);
}

void ClearPerGameConfig() {
    SwitchFrontend::RestoreGlobalSettings();
    s_per_game_id = 0;
}

std::uint64_t GetPerGameConfigId() {
    return s_per_game_id;
}

bool HasPerGameConfig(std::uint64_t program_id) {
    return program_id != 0 && FileUtil::Exists(PerGameConfigPath(program_id));
}

int CountPerGameOverrides() {
    if (s_per_game_id == 0) {
        return 0;
    }
    int count = 0;
    for (const SwitchFrontend::SettingEntry& entry : SwitchFrontend::Registry()) {
        count += entry.IsOverridden() ? 1 : 0;
    }
    return count;
}

const char* SettingsPresetName(SettingsPreset preset) {
    switch (preset) {
    case SettingsPreset::Performance:
        return "Performance";
    case SettingsPreset::UltraPerformance:
        return "Ultra Performance";
    default:
        return "Default";
    }
}

const char* SettingsPresetSummary(SettingsPreset preset) {
    switch (preset) {
    case SettingsPreset::Performance:
        return "Faster, at the cost of accuracy. Recommend unless things break.";
    case SettingsPreset::UltraPerformance:
        return "Fastest by breaking things. Expect breakage.";
    default:
        return "Default stettins that are shipped with every build.";
    }
}

void ResetSettings(SettingsPreset preset) {
    if (!s_config) {
        return;
    }
    s_config->ResetToDefaults();
    ApplyPreset(preset);

    Common::Log::Filter log_filter;
    log_filter.ParseFilterString(Settings::values.log_filter.GetValue());
    Common::Log::SetGlobalFilter(log_filter);

    RequestLayoutUpdate();
    s_config->Save();
    LOG_INFO(Config, "Settings reset to the {} preset", SettingsPresetName(preset));
}

void Shutdown() {
    s_config.reset();
    Common::Log::Stop();
}

} // namespace SwitchFrontend
