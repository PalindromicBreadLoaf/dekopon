// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <optional>
#include <type_traits>

#include "audio_core/dsp_interface.h"
#include "citra_switch/config.h"
#include "citra_switch/input.h"
#include "citra_switch/overlay_menu.h"
#include "citra_switch/settings_menu.h"
#include "common/logging/backend.h"
#include "common/logging/filter.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/service/cfg/cfg.h"

namespace SwitchFrontend {

namespace {

std::string BoolText(bool on) {
    return on ? "On" : "Off";
}

// The system language is kept in the CFG NAND savegame rather than config.ini, so it is read once
// and written back only when the user leaves the Settings tab.
std::optional<int> s_language;
bool s_language_dirty = false;

int ReadLanguage() {
    if (!s_language) {
        s_language = static_cast<int>(
            Service::CFG::GetModule(Core::System::GetInstance())->GetSystemLanguage());
    }
    return *s_language;
}

// Ordered to match Service::CFG::SystemLanguage.
constexpr std::array<const char*, 12> kLanguageNames{
    "Japanese",           "English", "French", "German",     "Italian", "Spanish",
    "Simplified Chinese", "Korean",  "Dutch",  "Portuguese", "Russian", "Traditional Chinese"};

// Ordered to match the SMDH region list, offset by one so index 0 is the auto-select sentinel.
constexpr std::array<const char*, 8> kRegionNames{"Auto",      "Japan", "USA",   "Europe",
                                                  "Australia", "China", "Korea", "Taiwan"};

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

SettingsRow LanguageRow() {
    return {
        "System Language",
        [] {
            const std::size_t index = static_cast<std::size_t>(ReadLanguage());
            return std::string{index < kLanguageNames.size() ? kLanguageNames[index] : "English"};
        },
        [](int dir) {
            s_language =
                std::clamp(ReadLanguage() + dir, 0, static_cast<int>(kLanguageNames.size()) - 1);
            s_language_dirty = true;
        }};
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

} // namespace

const char* SettingsPageName(SettingsPage page) {
    switch (page) {
    case SettingsPage::General:
        return "General";
    case SettingsPage::System:
        return "System";
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
        };
    case SettingsPage::System:
        return {
            Toggle("New 3DS Mode", v.is_new_3ds),
            RegionRow(),
            LanguageRow(),
            Toggle("LLE Applets", v.lle_applets),
            Toggle("Required Online LLE Modules", v.enable_required_online_lle_modules),
            Toggle("Region Free Patch", v.apply_region_free_patch),
            Choice("Clock Source", v.init_clock, kInitClockNames),
            ClockOffsetRow(),
            Number("Pedometer Steps Per Hour", v.steps_per_hour, 0, 10000, 100),
            Toggle("Plugin Loader", v.plugin_loader_enabled),
            Toggle("Allow Games To Change Plugin Loader", v.allow_plugin_loader),
        };
    case SettingsPage::Graphics:
        return {
            GraphicsApiRow(),
            Toggle("Hardware Shader", v.use_hw_shader),
            Toggle("Accurate Shader Multiplication", v.shaders_accurate_mul),
            Toggle("Shader JIT", v.use_shader_jit),
            Toggle("Async Shader Compilation", v.async_shader_compilation),
            Toggle("Disk Shader Cache", v.use_disk_shader_cache),
            Toggle("Async GPU (restart)", v.async_gpu_emulation),
            Toggle("Strict GPU Sync", v.strict_gpu_sync),
            Toggle("Async Presentation", v.async_presentation),
            Toggle("VSync", v.use_vsync),
            Toggle("Detect Display Refresh Rate", v.use_display_refresh_rate_detection),
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
            Number("Stereoscopic Depth", v.factor_3d, 0, 100, 5, "%"),
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
            Toggle("Deterministic Async Operations", v.deterministic_async_operations),
            Toggle("Delay Start For LLE Modules", v.delay_start_for_lle_modules),
            Toggle("Unique Data Console Type", v.toggle_unique_data_console_type),
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
            Number("Stereoscopic Depth", v.factor_3d, 0, 100, 5, "%"),
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

void CommitSettings() {
    SaveConfig();
    if (!s_language_dirty) {
        return;
    }
    s_language_dirty = false;
    auto cfg = Service::CFG::GetModule(Core::System::GetInstance());
    const auto language = static_cast<Service::CFG::SystemLanguage>(*s_language);
    if (cfg->GetSystemLanguage() != language) {
        cfg->SetSystemLanguage(language);
        cfg->UpdateConfigNANDSavegame();
    }
}

} // namespace SwitchFrontend
