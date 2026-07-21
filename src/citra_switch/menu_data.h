// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "citra_switch/config.h"

// Data the menu draws produced by the Azahar core.
namespace SwitchFrontend {

// What the high word of a title ID says a title is.
enum class TitleKind {
    Application,
    Demo,
    Update,
    AddOnContent, // DLC.
    System,
    Other,
};

// One scanned title, either a loose file under the ROM directory or a title installed into the
// emulated SD card.
struct GameEntry {
    std::string path;           // Absolute SD path passed to BootRom.
    std::string title;          // SMDH long title, or the file name / title ID as a fallback.
    std::string publisher;      // SMDH publisher, empty if unknown.
    std::string file_type;
    bool encrypted{};           // True if the SMDH couldn't be read due to missing keys.
    bool installed{};           // Came from the SD title tree rather than the ROM directory.
    std::uint64_t program_id{}; // 0 if it couldn't be read.
    int icon_size{};            // 48x48 RGBA8888 icon, empty if none.
    std::vector<std::uint32_t> icon;
};

// What is installed alongside a library entry. Gathered on demand since it costs a few TMD reads.
struct TitleDetails {
    std::uint64_t program_id{};
    TitleKind kind{};
    bool has_base_version{}; // Only an installed title carries a TMD to read a version out of.
    std::uint16_t base_version{};
    bool has_update{};
    std::uint16_t update_version{};
    bool has_dlc{};
    int dlc_contents{};
};

// One installable file listed by the CIA browser.
struct CiaEntry {
    std::string name; // Leaf file name.
    std::string path;
    std::uint64_t program_id{};
    TitleKind kind{};
    std::uint16_t version{};
    std::uint64_t size{};
    bool compressed{};
    bool readable{}; // False if the header or TMD wouldn't parse, which blocks installing it.
};

enum class InstallResult {
    Success,
    FileNotFound,
    FailedToOpen,
    Aborted,
    Invalid,
    Encrypted,
};

// One subfolder listed by the folder browser.
struct DirEntry {
    std::string name; // Leaf name, no trailing '/'.
    std::string path; // Absolute SD path with a trailing '/'.
};

// The subset of settings the on-console menu can edit.
struct MenuSettings {
    int resolution_factor{};      // 0 = auto (window), 1 = native, 2... = upscale factor.
    bool use_vsync{};
    bool async_shader_compilation{};
    bool use_disk_shader_cache{};
    bool use_hw_shader{};         // Off falls the whole pipeline back to the CPU shader engine.
    int texture_filter{};         // Upscaling filter, 0 = None .. 5 = MMPX.
    bool filter_mode{};           // Linear (on) or nearest-neighbour (off) screen sampling.
    bool use_integer_scaling{};   // Scale the screen by whole-number factors only.
    bool show_fps{};              // On-screen framerate counter.
    bool disable_right_eye_render{};
    bool preload_textures{};      // Load the whole custom texture pack up front.
    bool dump_textures{};         // Write the game's textures out to disk.
    int cpu_clock_percentage{};   // 5..400.
    bool is_new_3ds{};
    bool use_cpu_jit{};
    int region_value{};           // -1 = auto, 0..6 per SMDH region order.
    int language{};               // 3DS system language, 0..11.
    int graphics_api{};           // Active graphics API backend.
    int pointer_source{};         // Touch pointer driver.
    int gyro_sensitivity_x{};     // Gyro pointer horizontal sensitivity, percent of the default.
    int gyro_sensitivity_y{};     // Gyro pointer vertical sensitivity, percent of the default.
    std::uint32_t layout_cycle_mask{}; // Presets R3 cycles through (bit i = preset i).
    bool pause_in_quick_menu{};   // Freeze the game while the in-game quick menu is open.
};

// Scans the configured ROM directory and the installed SD titles, sorted by title.
std::vector<GameEntry> ScanGames();

// The version/update/DLC details behind a library entry.
TitleDetails GetTitleDetails(const GameEntry& entry);

// Classifies a title ID by its high word.
TitleKind ClassifyTitle(std::uint64_t program_id);

// A short name for `kind`, e.g. "Update".
const char* TitleKindName(TitleKind kind);

// Nicely formats a 3DS version string
std::string FormatTitleVersion(std::uint16_t version);

// The installed version of `program_id`, if that exact title is already on the emulated card.
bool GetInstalledVersion(std::uint64_t program_id, std::uint16_t& version);

// The .cia/.zcia files in `directory`, sorted by name.
std::vector<CiaEntry> ListCiaFiles(const std::string& directory);

// Installs `path` into the emulated NAND/SD title tree, calling `progress` with
// (bytes_written, total_bytes) as it runs.
InstallResult InstallCia(const std::string& path,
                         const std::function<void(std::size_t, std::size_t)>& progress);

// A short reason for a failed installation.
const char* InstallResultText(InstallResult result);

// The subfolders of `directory`, sorted by name.
std::vector<DirEntry> ListSubdirectories(const std::string& directory);

// The parent of `directory`, or "" if it is already a device root such as "sdmc:/".
std::string ParentDirectory(const std::string& directory);

// True if `directory` exists or could be created.
bool EnsureDirectory(const std::string& directory);

// Snapshots the editable settings from Settings::values.
MenuSettings GetMenuSettings();

// Applies edited settings to Settings::values and saves config.ini.
void SetMenuSettings(const MenuSettings& settings);

} // namespace SwitchFrontend
