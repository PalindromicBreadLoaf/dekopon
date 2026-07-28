// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <memory>
#include <sstream>
#include <string>
#include <type_traits>
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
#include "core/system_titles.h"

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

// Every Settings::values entry config.ini round-trips, in file order. Reading and writing share
// this list so the two cannot drift apart.
template <typename Visitor>
void VisitPersistedSettings(Visitor&& visit) {
    auto& v = Settings::values;

    visit("Core", v.use_cpu_jit);
    visit("Core", v.cpu_clock_percentage);
    visit("Core", v.is_new_3ds);

    visit("Renderer", v.graphics_api);
    visit("Renderer", v.use_gles);
    visit("Renderer", v.resolution_factor);
    visit("Renderer", v.use_integer_scaling);
    visit("Renderer", v.frame_limit);
    visit("Renderer", v.use_vsync);
    visit("Renderer", v.use_display_refresh_rate_detection);
    visit("Renderer", v.async_gpu_emulation);
    visit("Renderer", v.strict_gpu_sync);
    visit("Renderer", v.async_presentation);
    visit("Renderer", v.async_shader_compilation);
    visit("Renderer", v.use_disk_shader_cache);
    visit("Renderer", v.use_hw_shader);
    visit("Renderer", v.shaders_accurate_mul);
    visit("Renderer", v.use_shader_jit);
    visit("Renderer", v.spirv_shader_gen);
    visit("Renderer", v.disable_spirv_optimizer);
    visit("Renderer", v.texture_filter);
    visit("Renderer", v.texture_sampling);
    visit("Renderer", v.filter_mode);
    visit("Renderer", v.simulate_3ds_gpu_timings);
    visit("Renderer", v.delay_game_render_thread_us);
    visit("Renderer", v.show_fps);
    visit("Renderer", v.show_shader_compile_notice);
    visit("Renderer", v.render_3d);
    visit("Renderer", v.factor_3d);
    visit("Renderer", v.swap_eyes_3d);
    visit("Renderer", v.mono_render_option);
    visit("Renderer", v.disable_right_eye_render);

    visit("Layout", v.screen_gap);
    visit("Layout", v.large_screen_proportion);
    visit("Layout", v.overlay_screen_position);
    visit("Layout", v.overlay_screen_size);
    visit("Layout", v.overlay_screen_opacity);
    visit("Layout", v.screen_top_leftright_padding);
    visit("Layout", v.screen_top_topbottom_padding);
    visit("Layout", v.screen_bottom_leftright_padding);
    visit("Layout", v.screen_bottom_topbottom_padding);
    visit("Layout", v.bg_red);
    visit("Layout", v.bg_green);
    visit("Layout", v.bg_blue);
    visit("Layout", v.cardboard_screen_size);
    visit("Layout", v.cardboard_x_shift);
    visit("Layout", v.cardboard_y_shift);

    visit("Audio", v.audio_emulation);
    visit("Audio", v.volume);
    visit("Audio", v.enable_audio_stretching);
    visit("Audio", v.enable_realtime_audio);
    visit("Audio", v.simulate_headphones_plugged);

    visit("Utility", v.custom_textures);
    visit("Utility", v.preload_textures);
    visit("Utility", v.async_custom_loading);
    visit("Utility", v.dump_textures);

    visit("System", v.region_value);
    visit("System", v.lle_applets);
    visit("System", v.enable_required_online_lle_modules);
    visit("System", v.apply_region_free_patch);
    visit("System", v.init_clock);
    visit("System", v.init_time_offset);
    visit("System", v.steps_per_hour);
    visit("System", v.plugin_loader_enabled);
    visit("System", v.allow_plugin_loader);

    visit("Data Storage", v.use_virtual_sd);
    visit("Data Storage", v.compress_cia_installs);
    visit("Data Storage", v.async_fs_operations);

    visit("Debugging", v.deterministic_async_operations);
    visit("Debugging", v.delay_start_for_lle_modules);
    visit("Debugging", v.toggle_unique_data_console_type);
    visit("Debugging", v.break_on_unmapped_memory_access);
    visit("Debugging", v.renderer_debug);
    visit("Debugging", v.dump_command_buffers);
    visit("Debugging", v.instant_debug_log);

    visit("Miscellaneous", v.log_filter);
}

template <typename Type, bool ranged>
void WriteSetting(std::ostringstream& ss, const Settings::Setting<Type, ranged>& setting) {
    ss << setting.GetLabel() << " = ";
    if constexpr (std::is_same_v<Type, bool>) {
        ss << (setting.GetValue() ? "true" : "false");
    } else if constexpr (std::is_enum_v<Type>) {
        ss << static_cast<int>(setting.GetValue());
    } else {
        ss << setting.GetValue();
    }
    ss << '\n';
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
        Type default_value = setting.GetDefault();
        if constexpr (std::is_integral_v<Type> && !std::is_same_v<Type, bool>) {
            if (group == "Renderer" &&
                setting.GetLabel() == Settings::values.factor_3d.GetLabel()) {
                default_value = static_cast<Type>(60);
            } else if (group == "Layout" &&
                       setting.GetLabel() == Settings::values.cardboard_x_shift.GetLabel()) {
                default_value = static_cast<Type>(35);
            } else if (group == "Layout" &&
                       setting.GetLabel() == Settings::values.cardboard_y_shift.GetLabel()) {
                default_value = static_cast<Type>(0);
            }
        }

        if constexpr (std::is_same_v<Type, std::string>) {
            std::string value = config->Get(group, setting.GetLabel(), default_value);
            setting = value.empty() ? default_value : std::move(value);
        } else if constexpr (std::is_same_v<Type, bool>) {
            setting = config->GetBoolean(group, setting.GetLabel(), default_value);
        } else if constexpr (std::is_floating_point_v<Type>) {
            setting = static_cast<Type>(config->GetReal(group, setting.GetLabel(), default_value));
        } else {
            setting = static_cast<Type>(config->GetInteger(
                group, setting.GetLabel(), static_cast<long>(default_value)));
        }
    }

    void ReadValues() {
        VisitPersistedSettings(
            [this](const char* group, auto& setting) { ReadSetting(group, setting); });

        // New 3DS mode doubles the emulated core count and the JIT code caches that go with it.
        // This is just a performance loss if the game doesn't use these cores.
        Settings::values.is_new_3ds =
            config->GetBoolean("Core", Settings::values.is_new_3ds.GetLabel(), false);

        SwitchFrontend::SetFullscreenStretchEnabled(config->GetBoolean(
            "Switch", "stretch_fullscreen",
            config->GetBoolean("Layout", "stretch_fullscreen", false)));

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
        s_inserted_cartridge =
            Common::StripSpaces(config->Get("Switch", "inserted_cartridge", ""));
        if (!s_inserted_cartridge.empty() && !FileUtil::Exists(s_inserted_cartridge)) {
            s_inserted_cartridge.clear();
        }
        s_artic_base_address =
            Common::StripSpaces(config->Get("Switch", "last_artic_base_addr", ""));
        Settings::values.use_artic_base_controller = config->GetBoolean(
            "Controls", Settings::values.use_artic_base_controller.GetLabel(), false);

        SwitchFrontend::SetPointerSource(static_cast<SwitchFrontend::PointerSource>(
            std::clamp<long>(config->GetInteger("Switch", "pointer_source", 0), 0,
                             SwitchFrontend::NumPointerSources - 1)));
        SwitchFrontend::SetGyroSensitivity(
            config->GetInteger("Switch", "gyro_sensitivity_x", 100),
            config->GetInteger("Switch", "gyro_sensitivity_y", 100));

        SwitchFrontend::SetPauseInQuickMenu(
            config->GetBoolean("Switch", "pause_in_quick_menu", false));

        SwitchFrontend::SetMovieThrottleEnabled(
            config->GetBoolean("Experimental", "movie_cpu_throttle", false));
        SwitchFrontend::SetMovieThrottleClockPercentage(static_cast<std::int32_t>(
            config->GetInteger("Experimental", "movie_cpu_clock_percentage", 45)));

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

        std::string open_group;
        VisitPersistedSettings([&](const char* group, const auto& setting) {
            if (open_group != group) {
                open_group = group;
                ss << (ss.tellp() == 0 ? "" : "\n") << '[' << group << "]\n";
            }
            WriteSetting(ss, setting);
        });

        ss << "\n[Switch]\n";
        ss << "roms_dir = " << s_paths.roms_dir << '\n';
        ss << "scan_recursive = " << (s_paths.scan_recursive ? "true" : "false") << '\n';
        ss << "inserted_cartridge = " << s_inserted_cartridge << '\n';
        ss << "last_artic_base_addr = " << s_artic_base_address << '\n';
        ss << "stretch_fullscreen = "
           << (SwitchFrontend::IsFullscreenStretchEnabled() ? "true" : "false") << '\n';
        ss << "pointer_source = " << static_cast<int>(SwitchFrontend::GetPointerSource()) << '\n';
        ss << "gyro_sensitivity_x = " << SwitchFrontend::GetGyroSensitivityX() << '\n';
        ss << "gyro_sensitivity_y = " << SwitchFrontend::GetGyroSensitivityY() << '\n';
        ss << "layout_cycle_mask = " << SwitchFrontend::GetLayoutCycleMask() << '\n';
        ss << "pause_in_quick_menu = "
           << (SwitchFrontend::IsPauseInQuickMenu() ? "true" : "false") << '\n';
        ss << "launch_count = " << launch_count << "\n\n";

        ss << "[Experimental]\n";
        ss << "movie_cpu_throttle = "
           << (SwitchFrontend::IsMovieThrottleEnabled() ? "true" : "false") << '\n';
        ss << "movie_cpu_clock_percentage = " << SwitchFrontend::GetMovieThrottleClockPercentage()
           << "\n\n";

        ss << "[Controls]\n";
        WriteSetting(ss, Settings::values.use_artic_base_controller);
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

bool GetUseArticBaseController() {
    return Settings::values.use_artic_base_controller.GetValue();
}

void SetUseArticBaseController(bool enabled) {
    Settings::values.use_artic_base_controller = enabled;
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
