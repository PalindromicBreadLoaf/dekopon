// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <INIReader.h>
#include "common/file_util.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/string_util.h"
#include "citra_switch/config.h"
#include "citra_switch/default_ini.h"
#include "citra_switch/input.h"
#include "citra_switch/overlay_menu.h"
#include "core/hle/service/service.h"

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
        LoadINI(DefaultINI::sConfigFile);
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

private:
    std::unique_ptr<INIReader> config;
    std::string config_loc;
    int launch_count = 0;

    bool LoadINI(const std::string& default_contents, bool retry = true) {
        if (config == nullptr || config->ParseError() < 0) {
            if (retry) {
                LOG_WARNING(Config, "Failed to load {}. Creating file from defaults...", config_loc);
                FileUtil::CreateFullPath(config_loc);
                FileUtil::WriteStringToFile(true, config_loc, default_contents);
                std::string ini_buffer;
                FileUtil::ReadFileToString(true, config_loc, ini_buffer);
                config = std::make_unique<INIReader>(ini_buffer.c_str(), ini_buffer.size());
                return LoadINI(default_contents, false);
            }
            LOG_ERROR(Config, "Failed to load config from {}", config_loc);
            return false;
        }
        LOG_INFO(Config, "Successfully loaded {}", config_loc);
        return true;
    }

    template <typename Type, bool ranged>
    void ReadSetting(const std::string& group, Settings::Setting<Type, ranged>& setting) {
        if constexpr (std::is_same_v<Type, std::string>) {
            std::string value = config->Get(group, setting.GetLabel(), setting.GetDefault());
            setting = value.empty() ? setting.GetDefault() : std::move(value);
        } else if constexpr (std::is_same_v<Type, bool>) {
            setting = config->GetBoolean(group, setting.GetLabel(), setting.GetDefault());
        } else if constexpr (std::is_floating_point_v<Type>) {
            setting =
                static_cast<Type>(config->GetReal(group, setting.GetLabel(), setting.GetDefault()));
        } else {
            setting = static_cast<Type>(config->GetInteger(
                group, setting.GetLabel(), static_cast<long>(setting.GetDefault())));
        }
    }

    void ReadValues() {
        // Core
        ReadSetting("Core", Settings::values.use_cpu_jit);
        ReadSetting("Core", Settings::values.cpu_clock_percentage);
        Settings::values.is_new_3ds =
            config->GetBoolean("Core", Settings::values.is_new_3ds.GetLabel(), false);

        // Renderer
        ReadSetting("Renderer", Settings::values.graphics_api);
        ReadSetting("Renderer", Settings::values.use_gles);
        ReadSetting("Renderer", Settings::values.resolution_factor);
        ReadSetting("Renderer", Settings::values.use_vsync);
        ReadSetting("Renderer", Settings::values.async_gpu_emulation);
        ReadSetting("Renderer", Settings::values.strict_gpu_sync);
        ReadSetting("Renderer", Settings::values.async_shader_compilation);
        ReadSetting("Renderer", Settings::values.use_disk_shader_cache);
        ReadSetting("Renderer", Settings::values.use_hw_shader);
        ReadSetting("Renderer", Settings::values.texture_filter);
        ReadSetting("Renderer", Settings::values.filter_mode);
        ReadSetting("Renderer", Settings::values.use_integer_scaling);
        ReadSetting("Renderer", Settings::values.show_fps);
        ReadSetting("Renderer", Settings::values.show_shader_compile_notice);
        ReadSetting("Renderer", Settings::values.use_shader_jit);
        ReadSetting("Renderer", Settings::values.disable_right_eye_render);

        // Layout
        ReadSetting("Layout", Settings::values.screen_gap);
        ReadSetting("Layout", Settings::values.overlay_screen_position);
        ReadSetting("Layout", Settings::values.overlay_screen_size);
        ReadSetting("Layout", Settings::values.overlay_screen_opacity);

        // Utility
        ReadSetting("Utility", Settings::values.custom_textures);
        ReadSetting("Utility", Settings::values.preload_textures);
        ReadSetting("Utility", Settings::values.dump_textures);

        // System
        ReadSetting("System", Settings::values.region_value);

        // Miscellaneous
        ReadSetting("Miscellaneous", Settings::values.log_filter);

        // The core expects every known service module to have an explicit setting and crashes if not.
        for (const auto& service_module : Service::service_module_map) {
            Settings::values.lle_modules.emplace(service_module.name, false);
        }

        s_paths.roms_dir =
            WithTrailingSlash(Common::StripSpaces(config->Get("Switch", "roms_dir", "")));
        if (s_paths.roms_dir.empty()) {
            s_paths.roms_dir = SwitchFrontend::GetDefaultRomsDir(s_paths.user_dir);
        }
        s_paths.scan_recursive = config->GetBoolean("Switch", "scan_recursive", true);

        SwitchFrontend::SetPointerSource(static_cast<SwitchFrontend::PointerSource>(
            std::clamp<long>(config->GetInteger("Switch", "pointer_source", 0), 0,
                             SwitchFrontend::NumPointerSources - 1)));
        SwitchFrontend::SetGyroSensitivity(
            config->GetInteger("Switch", "gyro_sensitivity_x", 100),
            config->GetInteger("Switch", "gyro_sensitivity_y", 100));

        SwitchFrontend::SetPauseInQuickMenu(
            config->GetBoolean("Switch", "pause_in_quick_menu", false));

        const long all_layouts = (1L << SwitchFrontend::GetScreenLayoutCount()) - 1;
        SwitchFrontend::SetLayoutCycleMask(static_cast<std::uint32_t>(
            config->GetInteger("Switch", "layout_cycle_mask", all_layouts)));

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
        std::ostringstream ss;
        const auto& v = Settings::values;

        ss << "[Core]\n";
        ss << "use_cpu_jit = " << (v.use_cpu_jit.GetValue() ? "true" : "false") << '\n';
        ss << "cpu_clock_percentage = " << v.cpu_clock_percentage.GetValue() << '\n';
        ss << "is_new_3ds = " << (v.is_new_3ds.GetValue() ? "true" : "false") << "\n\n";

        ss << "[Renderer]\n";
        ss << "graphics_api = " << static_cast<int>(v.graphics_api.GetValue()) << '\n';
        ss << "use_gles = " << (v.use_gles.GetValue() ? "true" : "false") << '\n';
        ss << "resolution_factor = " << v.resolution_factor.GetValue() << '\n';
        ss << "use_vsync = " << (v.use_vsync.GetValue() ? "true" : "false") << '\n';
        ss << "async_gpu_emulation = " << (v.async_gpu_emulation.GetValue() ? "true" : "false")
           << '\n';
        ss << "strict_gpu_sync = " << (v.strict_gpu_sync.GetValue() ? "true" : "false") << '\n';
        ss << "async_shader_compilation = "
           << (v.async_shader_compilation.GetValue() ? "true" : "false") << '\n';
        ss << "use_disk_shader_cache = " << (v.use_disk_shader_cache.GetValue() ? "true" : "false")
           << '\n';
        ss << "use_hw_shader = " << (v.use_hw_shader.GetValue() ? "true" : "false") << '\n';
        ss << "texture_filter = " << static_cast<int>(v.texture_filter.GetValue()) << '\n';
        ss << "filter_mode = " << (v.filter_mode.GetValue() ? "true" : "false") << '\n';
        ss << "use_integer_scaling = " << (v.use_integer_scaling.GetValue() ? "true" : "false")
           << '\n';
        ss << "show_fps = " << (v.show_fps.GetValue() ? "true" : "false") << '\n';
        ss << "show_shader_compile_notice = "
           << (v.show_shader_compile_notice.GetValue() ? "true" : "false") << '\n';
        ss << "use_shader_jit = " << (v.use_shader_jit.GetValue() ? "true" : "false") << '\n';
        ss << "disable_right_eye_render = "
           << (v.disable_right_eye_render.GetValue() ? "true" : "false") << "\n\n";

        ss << "[Layout]\n";
        ss << "screen_gap = " << v.screen_gap.GetValue() << '\n';
        ss << "overlay_screen_position = "
           << static_cast<int>(v.overlay_screen_position.GetValue()) << '\n';
        ss << "overlay_screen_size = " << v.overlay_screen_size.GetValue() << '\n';
        ss << "overlay_screen_opacity = " << v.overlay_screen_opacity.GetValue() << "\n\n";

        ss << "[Utility]\n";
        ss << "custom_textures = " << (v.custom_textures.GetValue() ? "true" : "false") << '\n';
        ss << "preload_textures = " << (v.preload_textures.GetValue() ? "true" : "false") << '\n';
        ss << "dump_textures = " << (v.dump_textures.GetValue() ? "true" : "false") << "\n\n";

        ss << "[System]\n";
        ss << "region_value = " << v.region_value.GetValue() << "\n\n";

        ss << "[Miscellaneous]\n";
        ss << "log_filter = " << v.log_filter.GetValue() << "\n\n";

        ss << "[Switch]\n";
        ss << "roms_dir = " << s_paths.roms_dir << '\n';
        ss << "scan_recursive = " << (s_paths.scan_recursive ? "true" : "false") << '\n';
        ss << "pointer_source = " << static_cast<int>(SwitchFrontend::GetPointerSource()) << '\n';
        ss << "gyro_sensitivity_x = " << SwitchFrontend::GetGyroSensitivityX() << '\n';
        ss << "gyro_sensitivity_y = " << SwitchFrontend::GetGyroSensitivityY() << '\n';
        ss << "layout_cycle_mask = " << SwitchFrontend::GetLayoutCycleMask() << '\n';
        ss << "pause_in_quick_menu = "
           << (SwitchFrontend::IsPauseInQuickMenu() ? "true" : "false") << '\n';
        ss << "launch_count = " << launch_count << "\n\n";

        ss << "[Controls]\n";
        for (int i = 0; i < SwitchFrontend::NumMappableControls; ++i) {
            const auto control = static_cast<SwitchFrontend::MappableControl>(i);
            ss << SwitchFrontend::ControlConfigKey(control) << " = "
               << static_cast<int>(SwitchFrontend::GetMapping(control)) << '\n';
        }

        return ss.str();
    }
};

// Kept alive past Bootstrap() so the menu can re-save settings while preserving launch_count.
std::unique_ptr<Config> s_config;

} // namespace

namespace SwitchFrontend {

int Bootstrap() {
    // Resolve the dekopon directory and create its standard subdirectories.
    FileUtil::SetUserPath(ReadUserDirPointer());
    s_active_user_dir = FileUtil::GetUserPath(FileUtil::UserPath::UserDir);
    s_paths.user_dir = s_active_user_dir;

    Common::Log::Initialize();
    Common::Log::Start();

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
    LOG_INFO(Frontend, "Logging to: {}", FileUtil::GetUserPath(FileUtil::UserPath::LogDir));

    return s_config->LaunchCount();
}

const SwitchPaths& GetPaths() {
    return s_paths;
}

void SetPaths(const SwitchPaths& paths) {
    s_paths.roms_dir = WithTrailingSlash(paths.roms_dir);
    s_paths.scan_recursive = paths.scan_recursive;

    const std::string user_dir = WithTrailingSlash(paths.user_dir);
    if (user_dir != s_paths.user_dir) {
        s_paths.user_dir = user_dir;
        WriteUserDirPointer(user_dir);
        LOG_INFO(Frontend, "Dekopon directory set to {}, applies on the next launch", user_dir);
    }
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
}

void Shutdown() {
    s_config.reset();
    Common::Log::Stop();
}

} // namespace SwitchFrontend
