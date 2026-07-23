// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "citra_switch/config.h"
#include "citra_switch/input.h"
#include "citra_switch/overlay_menu.h"
#include "common/settings.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/loader/loader.h"
#include "video_core/overlay.h"

namespace SwitchFrontend {

namespace {

// The kind of row. Cheat rows are appended once per loaded cheat on the current page, so a row
// also carries the cheat's index into the engine's list.
enum class Item {
    ScreenLayout,
    SwapScreens,
    ScreenGap,
    OverlayPosition,
    OverlaySize,
    OverlayOpacity,
    FpsCounter,
    CustomTextures,
    DisableRightEyeRender,
    PointerSource,
    PointerMode,
    GyroSensitivityX,
    GyroSensitivityY,
    CpuClock,
    PauseInMenu,
    Cheat,
    CheatsEmpty,
    Resume,
    ExitGame,
};

struct Row {
    Item item;
    int cheat_index = -1;
};

// The overlay is split into pages with L/R used to cycle between them.
enum class Page {
    Display,
    Input,
    System,
    Cheats,
};

constexpr std::array<Page, 4> kPages = {Page::Display, Page::Input, Page::System, Page::Cheats};

const char* PageName(Page page) {
    switch (page) {
    case Page::Display:
        return "Display";
    case Page::Input:
        return "Input";
    case Page::System:
        return "System";
    case Page::Cheats:
        return "Cheats";
    }
    return "";
}

// Percentage step for the gyro sensitivity rows.
constexpr int kGyroStep = 10;

// Percentage step and range for the emulated CPU clock.
constexpr int kClockStep = 25;
constexpr int kClockMin = 25;
constexpr int kClockMax = 400;

// Cheats past this many spill onto further sub-pages.
constexpr int kCheatsPerPage = 8;

std::atomic<bool> s_open{false};
std::atomic<bool> s_pause_in_menu{false};
int s_page = 0;
int s_selected = 0;
int s_cheat_page = 0;
std::vector<Row> s_rows;
bool s_cheats_dirty = false;

Page CurrentPage() {
    return kPages[static_cast<std::size_t>(s_page)];
}

Cheats::CheatEngine* GetCheatEngine() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return nullptr;
    }
    return &system.CheatEngine();
}

int CheatCount() {
    auto* engine = GetCheatEngine();
    return engine ? static_cast<int>(engine->GetCheats().size()) : 0;
}

int CheatPageCount() {
    const int count = CheatCount();
    return count <= 0 ? 1 : (count + kCheatsPerPage - 1) / kCheatsPerPage;
}

std::string CheatName(int index) {
    auto* engine = GetCheatEngine();
    if (!engine) {
        return "";
    }
    const auto cheats = engine->GetCheats();
    return index >= 0 && index < static_cast<int>(cheats.size()) ? cheats[index]->GetName() : "";
}

bool CheatEnabled(int index) {
    auto* engine = GetCheatEngine();
    if (!engine) {
        return false;
    }
    const auto cheats = engine->GetCheats();
    return index >= 0 && index < static_cast<int>(cheats.size()) && cheats[index]->IsEnabled();
}

void ToggleCheat(int index) {
    auto* engine = GetCheatEngine();
    if (!engine) {
        return;
    }
    const auto cheats = engine->GetCheats();
    if (index < 0 || index >= static_cast<int>(cheats.size())) {
        return;
    }
    cheats[index]->SetEnabled(!cheats[index]->IsEnabled());
    s_cheats_dirty = true;
}

// Writes the enabled state the player just picked back to the cheat file so it sticks.
void PersistCheats() {
    if (!s_cheats_dirty) {
        return;
    }
    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn()) {
        u64 title_id = 0;
        system.GetAppLoader().ReadProgramId(title_id);
        system.CheatEngine().SaveCheatFile(title_id);
    }
    s_cheats_dirty = false;
}

// Rebuilds the visible rows for the active page and keeps the cursor in range.
void RebuildRows() {
    s_rows.clear();
    switch (CurrentPage()) {
    case Page::Display:
        s_rows.push_back({Item::ScreenLayout});
        s_rows.push_back({Item::SwapScreens});
        s_rows.push_back({Item::ScreenGap});
        if (IsOverlayScreenLayout()) {
            s_rows.push_back({Item::OverlayPosition});
            s_rows.push_back({Item::OverlaySize});
            s_rows.push_back({Item::OverlayOpacity});
        }
        s_rows.push_back({Item::FpsCounter});
        s_rows.push_back({Item::CustomTextures});
        s_rows.push_back({Item::DisableRightEyeRender});
        break;
    case Page::Input:
        s_rows.push_back({Item::PointerSource});
        s_rows.push_back({Item::PointerMode});
        s_rows.push_back({Item::GyroSensitivityX});
        s_rows.push_back({Item::GyroSensitivityY});
        break;
    case Page::System:
        s_rows.push_back({Item::CpuClock});
        s_rows.push_back({Item::PauseInMenu});
        s_rows.push_back({Item::Resume});
        s_rows.push_back({Item::ExitGame});
        break;
    case Page::Cheats: {
        const int count = CheatCount();
        s_cheat_page = std::clamp(s_cheat_page, 0, CheatPageCount() - 1);
        const int first = s_cheat_page * kCheatsPerPage;
        const int last = std::min(count, first + kCheatsPerPage);
        for (int i = first; i < last; ++i) {
            s_rows.push_back({Item::Cheat, i});
        }
        if (s_rows.empty()) {
            s_rows.push_back({Item::CheatsEmpty});
        }
        break;
    }
    }
    s_selected = std::clamp(s_selected, 0, static_cast<int>(s_rows.size()) - 1);
}

// The running timers keep their own copy of the clock scale, so a change has to be pushed into
// core timing to take effect without a reboot.
void SetCpuClock(int percent) {
    percent = std::clamp(percent, kClockMin, kClockMax);
    Settings::values.cpu_clock_percentage = percent;
    auto& system = Core::System::GetInstance();
    if (system.IsPoweredOn()) {
        system.CoreTiming().UpdateClockSpeed(static_cast<u32>(percent));
    }
}

bool IsAction(const Row& row) {
    return row.item == Item::SwapScreens || row.item == Item::Resume ||
           row.item == Item::ExitGame || row.item == Item::CheatsEmpty;
}

std::string Label(const Row& row) {
    switch (row.item) {
    case Item::ScreenLayout:
        return "Screen Layout";
    case Item::SwapScreens:
        return "Swap Screens";
    case Item::ScreenGap:
        return "Screen Gap";
    case Item::OverlayPosition:
        return "Overlay Position";
    case Item::OverlaySize:
        return "Overlay Size";
    case Item::OverlayOpacity:
        return "Overlay Opacity";
    case Item::GyroSensitivityX:
        return "Gyro Sens. X";
    case Item::GyroSensitivityY:
        return "Gyro Sens. Y";
    case Item::PointerSource:
        return "Touch Pointer";
    case Item::PointerMode:
        return "Pointer Mode";
    case Item::FpsCounter:
        return "FPS Counter";
    case Item::CustomTextures:
        return "Custom Textures";
    case Item::DisableRightEyeRender:
        return "No Right Eye";
    case Item::CpuClock:
        return "CPU Clock";
    case Item::PauseInMenu:
        return "Pause In Menu";
    case Item::Cheat:
        return CheatName(row.cheat_index);
    case Item::CheatsEmpty:
        return "No cheats loaded";
    case Item::Resume:
        return "Resume Game";
    case Item::ExitGame:
        return "Exit to Library";
    }
    return "";
}

std::string Value(const Row& row) {
    switch (row.item) {
    case Item::ScreenLayout:
        return CurrentScreenLayoutName();
    case Item::ScreenGap:
        return std::to_string(GetScreenGap()) + " px";
    case Item::OverlayPosition:
        return OverlayScreenPositionName();
    case Item::OverlaySize:
        return std::to_string(GetOverlayScreenSize()) + "%";
    case Item::OverlayOpacity:
        return std::to_string(GetOverlayScreenOpacity()) + "%";
    case Item::GyroSensitivityX:
        return std::to_string(GetGyroSensitivityX()) + "%";
    case Item::GyroSensitivityY:
        return std::to_string(GetGyroSensitivityY()) + "%";
    case Item::PointerSource:
        return PointerSourceName(GetPointerSource());
    case Item::PointerMode:
        return IsPointerModeActive() ? "On" : "Off";
    case Item::FpsCounter:
        return Settings::values.show_fps.GetValue() ? "On" : "Off";
    case Item::CustomTextures:
        return Settings::values.custom_textures.GetValue() ? "On" : "Off";
    case Item::DisableRightEyeRender:
        return Settings::values.disable_right_eye_render.GetValue() ? "On" : "Off";
    case Item::CpuClock:
        return std::to_string(Settings::values.cpu_clock_percentage.GetValue()) + "%";
    case Item::PauseInMenu:
        return IsPauseInQuickMenu() ? "On" : "Off";
    case Item::Cheat:
        return CheatEnabled(row.cheat_index) ? "On" : "Off";
    default:
        return "";
    }
}

// Left/right on a value row. `dir` is -1 or +1.
void Adjust(const Row& row, int dir) {
    switch (row.item) {
    case Item::ScreenLayout:
        StepScreenLayout(dir);
        break;
    case Item::ScreenGap:
        StepScreenGap(dir);
        break;
    case Item::OverlayPosition:
        StepOverlayScreenPosition(dir);
        break;
    case Item::OverlaySize:
        StepOverlayScreenSize(dir);
        break;
    case Item::OverlayOpacity:
        StepOverlayScreenOpacity(dir);
        break;
    case Item::GyroSensitivityX:
        SetGyroSensitivity(GetGyroSensitivityX() + dir * kGyroStep, GetGyroSensitivityY());
        break;
    case Item::GyroSensitivityY:
        SetGyroSensitivity(GetGyroSensitivityX(), GetGyroSensitivityY() + dir * kGyroStep);
        break;
    case Item::PointerSource:
        SetPointerSource(static_cast<PointerSource>(std::clamp(
            static_cast<int>(GetPointerSource()) + dir, 0, NumPointerSources - 1)));
        break;
    case Item::PointerMode:
        SetPointerMode(dir > 0);
        break;
    case Item::FpsCounter:
        Settings::values.show_fps = dir > 0;
        break;
    case Item::CustomTextures:
        Settings::values.custom_textures = dir > 0;
        break;
    case Item::DisableRightEyeRender:
        Settings::values.disable_right_eye_render = dir > 0;
        break;
    case Item::CpuClock:
        SetCpuClock(Settings::values.cpu_clock_percentage.GetValue() + dir * kClockStep);
        break;
    case Item::PauseInMenu:
        SetPauseInQuickMenu(dir > 0);
        break;
    case Item::Cheat:
        ToggleCheat(row.cheat_index);
        break;
    default:
        break;
    }
}

// Pressing 'a' on a row advances the list by one.
void Activate(const Row& row) {
    switch (row.item) {
    case Item::SwapScreens:
        ToggleSwapScreens();
        break;
    case Item::PointerSource:
        SetPointerSource(static_cast<PointerSource>(
            (static_cast<int>(GetPointerSource()) + 1) % NumPointerSources));
        break;
    case Item::PointerMode:
        TogglePointerMode();
        break;
    case Item::FpsCounter:
        Settings::values.show_fps = !Settings::values.show_fps.GetValue();
        break;
    case Item::CustomTextures:
        Settings::values.custom_textures = !Settings::values.custom_textures.GetValue();
        break;
    case Item::DisableRightEyeRender:
        Settings::values.disable_right_eye_render =
            !Settings::values.disable_right_eye_render.GetValue();
        break;
    case Item::PauseInMenu:
        SetPauseInQuickMenu(!IsPauseInQuickMenu());
        break;
    case Item::Cheat:
        ToggleCheat(row.cheat_index);
        break;
    default:
        Adjust(row, 1);
        break;
    }
}

void Repaint() {
    VideoCore::OverlayMenuState state;
    state.visible = s_open.load(std::memory_order_relaxed);
    state.selected = s_selected;
    state.title = std::string("Quick Menu - ") + PageName(CurrentPage()) + " (" +
                  std::to_string(s_page + 1) + "/" + std::to_string(kPages.size()) + ")";
    if (CurrentPage() == Page::Cheats) {
        const int cheat_pages = CheatPageCount();
        if (cheat_pages > 1) {
            state.title += "  List " + std::to_string(s_cheat_page + 1) + "/" +
                           std::to_string(cheat_pages);
            state.hint = "A Toggle   L/R Page   ZL/ZR List   +/- Close";
        } else {
            state.hint = "A Toggle   L/R Page   +/- Close";
        }
    } else {
        state.hint = "A Change   L/R Page   +/- Close";
    }
    state.items.reserve(s_rows.size());
    for (const Row& row : s_rows) {
        state.items.push_back({Label(row), Value(row), IsAction(row)});
    }
    VideoCore::SetOverlayMenuState(state);
}

} // namespace

bool IsQuickMenuOpen() {
    return s_open.load(std::memory_order_relaxed);
}

bool IsPauseInQuickMenu() {
    return s_pause_in_menu.load(std::memory_order_relaxed);
}

void SetPauseInQuickMenu(bool enabled) {
    s_pause_in_menu.store(enabled, std::memory_order_relaxed);
    if (IsQuickMenuOpen()) {
        SetEmulationPaused(enabled);
    }
}

void OpenQuickMenu() {
    s_page = 0;
    s_selected = 0;
    s_cheat_page = 0;
    RebuildRows();
    s_open.store(true, std::memory_order_relaxed);
    if (IsPauseInQuickMenu()) {
        SetEmulationPaused(true);
    }
    Repaint();
}

void CloseQuickMenu() {
    const bool was_open = s_open.exchange(false, std::memory_order_relaxed);
    SetEmulationPaused(false);
    VideoCore::OverlayMenuState state;
    state.visible = false;
    VideoCore::SetOverlayMenuState(state);
    // Persist the settings the player changed.
    if (was_open) {
        PersistCheats();
        SaveConfig();
    }
}

void ToggleQuickMenu() {
    if (IsQuickMenuOpen()) {
        CloseQuickMenu();
    } else {
        OpenQuickMenu();
    }
}

QuickMenuAction UpdateQuickMenu(const QuickMenuNav& nav) {
    if (!IsQuickMenuOpen()) {
        return QuickMenuAction::None;
    }

    if (nav.cancel) {
        CloseQuickMenu();
        return QuickMenuAction::Close;
    }

    bool changed = false;

    // L/R cycle through the menu's pages.
    if (nav.tab_prev != nav.tab_next) {
        const int count = static_cast<int>(kPages.size());
        s_page = (s_page + (nav.tab_next ? 1 : -1) + count) % count;
        s_selected = 0;
        RebuildRows();
        changed = true;
    }

    // ZL/ZR page through the cheat list.
    if (CurrentPage() == Page::Cheats && (nav.page_prev || nav.page_next)) {
        const int pages = CheatPageCount();
        const int dir = nav.page_next ? 1 : -1;
        s_cheat_page = (s_cheat_page + dir + pages) % pages;
        s_selected = 0;
        RebuildRows();
        changed = true;
    }

    const int count = static_cast<int>(s_rows.size());
    if (nav.up) {
        s_selected = (s_selected - 1 + count) % count;
        changed = true;
    }
    if (nav.down) {
        s_selected = (s_selected + 1) % count;
        changed = true;
    }

    const Row row = s_rows[s_selected];
    if (nav.left && !IsAction(row)) {
        Adjust(row, -1);
        changed = true;
    }
    if (nav.right && !IsAction(row)) {
        Adjust(row, 1);
        changed = true;
    }
    if (nav.confirm) {
        if (row.item == Item::Resume) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        if (row.item == Item::ExitGame) {
            CloseQuickMenu();
            return QuickMenuAction::ExitGame;
        }
        Activate(row);
        changed = true;
    }

    if (changed) {
        RebuildRows();
        Repaint();
    }
    return QuickMenuAction::None;
}

} // namespace SwitchFrontend
