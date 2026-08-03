// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>

// Facade between the <switch.h> main() and the Azahar core.
namespace SwitchFrontend {

// Directories the frontend owns. Absolute SD paths including a trailing '/'.
struct SwitchPaths {
    std::string user_dir;   // Holds config/, nand/, sdmc/, log/, ...
    std::string roms_dir;   // Scanned for titles.
    std::string roms_dir_2; // Another location for titles.
    bool scan_recursive{};  // Whether the scan descends into the ROM folders' subfolders.
};

struct SystemFileSetupState {
    bool old3ds{};
    bool new3ds{};
};

enum class SystemFileSetupMode {
    Old3ds,
    New3ds,
};

// Sets up default directory and logging on first boot.
int Bootstrap();

// The configured paths.
const SwitchPaths& GetPaths();

// Persists `paths`. The ROM folders and scan_recursive apply to the next scan.
void SetPaths(const SwitchPaths& paths);

// Cartridge image exposed to software launched from the emulated HOME Menu.
const std::string& GetInsertedCartridge();
void SetInsertedCartridge(const std::string& path);

// Last Artic Base/Artic Setup Tool address entered in the launcher.
const std::string& GetArticBaseAddress();
void SetArticBaseAddress(const std::string& address);

SystemFileSetupState GetSystemFileSetupState();
void PrepareSystemFileSetup(SystemFileSetupMode mode);

bool GetUseArticBaseController();
void SetUseArticBaseController(bool enabled);

// Which of the 3DS's three cameras the picked image feeds.
enum class CameraTarget {
    All,
    Outer,
    Inner,
};

constexpr int NumCameraTargets = 3;

const char* CameraTargetName(CameraTarget target);

// The image file the emulated cameras hand to the guest.
const std::string& GetCameraImage();
CameraTarget GetCameraTarget();

// Points the cameras at `path`.
void SetCameraImage(const std::string& path, CameraTarget target);

// Clockwise rotation of the launcher and the in-game overlay, in degrees.
int GetMenuRotation();
void SetMenuRotation(int degrees);

// Whether the d-pad turns with the menu.
bool IsMenuInputRotated();
void SetMenuInputRotated(bool enabled);

struct MenuDirections {
    bool up{};
    bool down{};
    bool left{};
    bool right{};
};

// Maps pressed d-pad directions onto the rotated menu.
MenuDirections RotateMenuDirections(MenuDirections pressed);

// The dekopon directory this session actually booted from.
const std::string& GetActiveUserDir();

// The built-in locations offered as a reset target in the UI.
std::string GetDefaultUserDir();
std::string GetDefaultRomsDir(const std::string& user_dir);

// Serialises the current Settings::values back to config.ini.
void SaveConfig();

// The bundles the reset row can put the settings into.
enum class SettingsPreset {
    Default,
    Performance,
    UltraPerformance,
};

constexpr int NumSettingsPresets = 3;

const char* SettingsPresetName(SettingsPreset preset);

// One-line description shown under the preset's name in the picker.
const char* SettingsPresetSummary(SettingsPreset preset);

// Puts everything the Settings tab covers back to defaults, then applies `preset` on top.
void ResetSettings(SettingsPreset preset);

// Flushes and stops the logger.
void Shutdown();

// Brings up the EGL/GLES EmuWindow on the given libnx nwindow.
bool CreateWindow(void* native_window);

// Clears the window to a solid colour and presents.
// This will be removed in the future once a UI is established
void ClearFrame();

// Tears down the EmuWindow and its GL context.
void DestroyWindow();

// Resolves a ROM and starts it
bool BootRom(const std::string& rom_arg);

// True while the emulation thread is running.
bool IsRunning();

// Freezes the guest between run loop slices, while the frontend keeps presenting the last frame.
void SetEmulationPaused(bool paused);
bool IsEmulationPaused();

// Advances the screen layout to the next preset while a game runs.
void CycleScreenLayout();

// Steps the screen layout by `delta` presets and applies it live.
void StepScreenLayout(int delta);

// Swaps which 3DS screen occupies which slot of the current arrangement.
void ToggleSwapScreens();

// Stretches either single-screen layout to fill the entire display.
bool IsFullscreenStretchEnabled();
void SetFullscreenStretchEnabled(bool enabled);

// True while the layout draws one screen as an overlay on top of the other.
bool IsOverlayScreenLayout();

// Live adjustments for the overlaid screen.
void StepOverlayScreenPosition(int delta);
const char* OverlayScreenPositionName();
void StepOverlayScreenSize(int delta);
int GetOverlayScreenSize();
void StepOverlayScreenOpacity(int delta);
int GetOverlayScreenOpacity();

// Live gap between the two screens, in px relative to the larger screen.
void StepScreenGap(int delta);
int GetScreenGap();

// Asks the emulation thread to re-derive the framebuffer layout, for settings the layout is
// computed from but that are not stepped through the helpers above.
void RequestLayoutUpdate();

// The name of the currently selected screen layout preset.
const char* CurrentScreenLayoutName();

// The number of screen layout presets R3 and the quick menu can select.
int GetScreenLayoutCount();

// The display name of preset `index` or "" if out of range.
const char* GetScreenLayoutName(int index);

// Bitmask of presets included in R3's cycle (bit i = preset i).
std::uint32_t GetLayoutCycleMask();
void SetLayoutCycleMask(std::uint32_t mask);

// Experimental movie playback throttle. Disabled by default.
bool IsMovieThrottleEnabled();
void SetMovieThrottleEnabled(bool enabled);
std::int32_t GetMovieThrottleClockPercentage();
void SetMovieThrottleClockPercentage(std::int32_t percentage);

// Queues a save state operation on `slot`. The emulation thread performs it at the next safe
// point and reports the outcome as an on-screen toast.
bool RequestSaveState(unsigned int slot);
bool RequestLoadState(unsigned int slot);

// Removes the state in `slot`, if any.
bool DeleteSaveState(unsigned int slot);

// Display name for a save state slot.
std::string SaveStateSlotName(unsigned int slot);

// When `slot` holds a state, its creation time as "DD/MM HH:MM" plus a marker when it came from
// a different build. Empty when the slot is free.
std::string SaveStateSlotStatus(unsigned int slot);

// True if the most recent BootRom never reached a successful system.Load.
bool LoadFailed();

// True if the most recent session failed because the Artic server disconnected or was unreachable.
bool ArticDisconnected();

// Signals the emulation thread to stop.
void StopRom();

} // namespace SwitchFrontend
