// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "citra_switch/config.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/overlay_menu.h"
#include "citra_switch/settings_menu.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/savestate.h"
#include "video_core/overlay.h"

namespace SwitchFrontend {

namespace {

// The kind of row. Setting rows index into the current page's row list, cheat rows into the
// engine's cheat list, and save state rows into the slots.
enum class Item {
    Setting,
    Cheat,
    CheatsEmpty,
    SaveStateSlot,
    AmiiboFile,
    AmiibosEmpty,
    RemoveAmiibo,
    Resume,
    ExitGame,
};

struct Row {
    Item item;
    int index = -1;
};

// The overlay is split into pages with L/R used to cycle between them. Everything up to States
// is a settings page and maps onto the QuickPage of the same ordinal.
enum class Page {
    Display,
    Graphics,
    Stereo,
    Audio,
    Input,
    System,
    States,
    Cheats,
    Amiibo,
};

constexpr std::array<Page, NumQuickPages + 3> kPages = {
    Page::Display, Page::Graphics, Page::Stereo, Page::Audio,  Page::Input,
    Page::System,  Page::States,   Page::Cheats, Page::Amiibo,
};

static_assert(static_cast<int>(Page::States) == NumQuickPages,
              "the settings pages have to lead, in QuickPage order");

bool IsSettingsPage(Page page) {
    return static_cast<int>(page) < NumQuickPages;
}

const char* PageName(Page page) {
    switch (page) {
    case Page::States:
        return "States";
    case Page::Cheats:
        return "Cheats";
    case Page::Amiibo:
        return "Amiibo";
    default:
        return QuickPageName(static_cast<QuickPage>(page));
    }
}

// Cheats past this many spill onto further sub-pages.
constexpr int kCheatsPerPage = 8;

// Save state slots past this many spill onto further sub-pages.
constexpr int kSlotsPerPage = 8;

constexpr int kAmiibosPerPage = 7;

std::atomic<bool> s_open{false};
std::atomic<bool> s_pause_in_menu{false};
int s_page = 0;
int s_selected = 0;
int s_cheat_page = 0;
int s_state_page = 0;
int s_amiibo_page = 0;
std::vector<Row> s_rows;
std::vector<SettingsRow> s_settings;
std::vector<AmiiboEntry> s_amiibos;
bool s_cheats_dirty = false;

// Slot status strings.
// Refreshed on demand so repaints don't stat the state directory.
std::array<std::string, Core::SaveStateSlotCount> s_slot_status;

void RefreshSaveStates() {
    for (u32 slot = 0; slot < Core::SaveStateSlotCount; ++slot) {
        s_slot_status[slot] = SaveStateSlotStatus(slot);
    }
}

int StatePageCount() {
    return (static_cast<int>(Core::SaveStateSlotCount) + kSlotsPerPage - 1) / kSlotsPerPage;
}

void RefreshAmiibos() {
    s_amiibos = ListAmiiboFiles();
}

int AmiiboPageCount() {
    const int count = static_cast<int>(s_amiibos.size());
    return count <= 0 ? 1 : (count + kAmiibosPerPage - 1) / kAmiibosPerPage;
}

bool LoadAmiibo(const std::string& path) {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        VideoCore::PostOverlayToast("No game is running");
        return false;
    }
    auto nfc = system.ServiceManager().GetService<Service::NFC::Module::Interface>("nfc:u");
    if (!nfc) {
        VideoCore::PostOverlayToast("The NFC service is unavailable");
        return false;
    }

    std::scoped_lock lock{system.Kernel().GetHLELock()};
    if (nfc->IsTagActive()) {
        VideoCore::PostOverlayToast("An Amiibo is already active");
        return false;
    }
    if (!nfc->IsSearchingForAmiibos()) {
        VideoCore::PostOverlayToast("The game is not scanning for Amiibo");
        return false;
    }
    if (!nfc->LoadAmiibo(path)) {
        VideoCore::PostOverlayToast("Could not load the Amiibo file");
        return false;
    }
    VideoCore::PostOverlayToast("Amiibo loaded");
    return true;
}

void RemoveAmiibo() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        VideoCore::PostOverlayToast("No game is running");
        return;
    }
    auto nfc = system.ServiceManager().GetService<Service::NFC::Module::Interface>("nfc:u");
    if (!nfc) {
        VideoCore::PostOverlayToast("The NFC service is unavailable");
        return;
    }

    std::scoped_lock lock{system.Kernel().GetHLELock()};
    if (!nfc->IsTagActive()) {
        VideoCore::PostOverlayToast("No Amiibo is active");
        return;
    }
    nfc->RemoveAmiibo();
    VideoCore::PostOverlayToast("Amiibo removed");
}

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
    s_settings.clear();
    const Page page = CurrentPage();
    if (IsSettingsPage(page)) {
        s_settings = BuildQuickPage(static_cast<QuickPage>(page));
        for (int i = 0; i < static_cast<int>(s_settings.size()); ++i) {
            s_rows.push_back({Item::Setting, i});
        }
        if (page == Page::System) {
            s_rows.push_back({Item::Resume});
            s_rows.push_back({Item::ExitGame});
        }
    } else if (page == Page::States) {
        s_state_page = std::clamp(s_state_page, 0, StatePageCount() - 1);
        const int first = s_state_page * kSlotsPerPage;
        const int last =
            std::min(static_cast<int>(Core::SaveStateSlotCount), first + kSlotsPerPage);
        for (int slot = first; slot < last; ++slot) {
            s_rows.push_back({Item::SaveStateSlot, slot});
        }
    } else if (page == Page::Cheats) {
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
    } else {
        s_amiibo_page = std::clamp(s_amiibo_page, 0, AmiiboPageCount() - 1);
        const int first = s_amiibo_page * kAmiibosPerPage;
        const int last = std::min(static_cast<int>(s_amiibos.size()), first + kAmiibosPerPage);
        for (int i = first; i < last; ++i) {
            s_rows.push_back({Item::AmiiboFile, i});
        }
        if (s_amiibos.empty()) {
            s_rows.push_back({Item::AmiibosEmpty});
        }
        s_rows.push_back({Item::RemoveAmiibo});
    }
    s_selected = std::clamp(s_selected, 0, static_cast<int>(s_rows.size()) - 1);
}

// Rows that only respond to A, and as such carry no value.
bool IsAction(const Row& row) {
    switch (row.item) {
    case Item::Setting:
    case Item::Cheat:
        return false;
    default:
        return true;
    }
}

std::string Label(const Row& row) {
    switch (row.item) {
    case Item::Setting:
        return s_settings[static_cast<std::size_t>(row.index)].label;
    case Item::Cheat:
        return CheatName(row.index);
    case Item::CheatsEmpty:
        return "No cheats loaded";
    case Item::SaveStateSlot:
        return SaveStateSlotName(static_cast<unsigned int>(row.index));
    case Item::AmiiboFile:
        return s_amiibos[static_cast<std::size_t>(row.index)].name;
    case Item::AmiibosEmpty:
        return "No .bin files in amiibo/";
    case Item::RemoveAmiibo:
        return "Remove active Amiibo";
    case Item::Resume:
        return "Resume Game";
    case Item::ExitGame:
        return "Exit to Library";
    }
    return "";
}

std::string Value(const Row& row) {
    switch (row.item) {
    case Item::Setting:
        return s_settings[static_cast<std::size_t>(row.index)].value();
    case Item::Cheat:
        return CheatEnabled(row.index) ? "On" : "Off";
    case Item::SaveStateSlot: {
        const std::string& status = s_slot_status[static_cast<std::size_t>(row.index)];
        return status.empty() ? "Empty" : status;
    }
    default:
        return "";
    }
}

// Left/right on a value row. `dir` is -1 or +1.
void Adjust(const Row& row, int dir) {
    switch (row.item) {
    case Item::Setting:
        s_settings[static_cast<std::size_t>(row.index)].step(dir);
        break;
    case Item::Cheat:
        ToggleCheat(row.index);
        break;
    default:
        break;
    }
}

// Pressing 'a' on a row advances the list by one, or flips it when it is an On/Off row.
void Activate(const Row& row) {
    if (row.item != Item::Setting) {
        Adjust(row, 1);
        return;
    }
    const SettingsRow& setting = s_settings[static_cast<std::size_t>(row.index)];
    setting.step(setting.boolean && setting.boolean() ? -1 : 1);
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
    } else if (CurrentPage() == Page::States) {
        const int state_pages = StatePageCount();
        if (state_pages > 1) {
            state.title +=
                "  List " + std::to_string(s_state_page + 1) + "/" + std::to_string(state_pages);
        }
        state.hint = "A Load   X Save   Y Delete   ZL/ZR List   L/R Page";
    } else if (CurrentPage() == Page::Amiibo) {
        const int amiibo_pages = AmiiboPageCount();
        if (amiibo_pages > 1) {
            state.title +=
                "  List " + std::to_string(s_amiibo_page + 1) + "/" + std::to_string(amiibo_pages);
            state.hint = "A Load/Remove   L/R Page   ZL/ZR List";
        } else {
            state.hint = "A Load/Remove   L/R Page";
        }
    } else {
        state.hint = "A Change   L/R Page   +/- Close";
    }
    state.items.reserve(s_rows.size());
    for (const Row& row : s_rows) {
        std::string value = Value(row);
        const bool no_value = value.empty();
        state.items.push_back({Label(row), std::move(value), no_value});
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
    s_state_page = 0;
    s_amiibo_page = 0;
    RefreshSaveStates();
    RefreshAmiibos();
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
        if (CurrentPage() == Page::States) {
            RefreshSaveStates();
        } else if (CurrentPage() == Page::Amiibo) {
            RefreshAmiibos();
        }
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

    // ZL/ZR page through the save state slots.
    if (CurrentPage() == Page::States && (nav.page_prev || nav.page_next)) {
        const int pages = StatePageCount();
        const int dir = nav.page_next ? 1 : -1;
        s_state_page = (s_state_page + dir + pages) % pages;
        s_selected = 0;
        RebuildRows();
        changed = true;
    }

    if (CurrentPage() == Page::Amiibo && (nav.page_prev || nav.page_next)) {
        const int pages = AmiiboPageCount();
        const int dir = nav.page_next ? 1 : -1;
        s_amiibo_page = (s_amiibo_page + dir + pages) % pages;
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
    // A loads, X saves, Y deletes. Save and load are queued for the emulation thread, which only
    // reaches them once the menu lets the guest run again, so close on the way out.
    if (row.item == Item::SaveStateSlot && (nav.confirm || nav.alt || nav.alt2)) {
        const auto slot = static_cast<unsigned int>(row.index);
        const bool occupied = !s_slot_status[slot].empty();
        if (nav.alt) {
            if (RequestSaveState(slot)) {
                CloseQuickMenu();
                return QuickMenuAction::Close;
            }
        } else if (nav.confirm) {
            if (!occupied) {
                VideoCore::PostOverlayToast(SaveStateSlotName(slot) + " is empty");
            } else if (RequestLoadState(slot)) {
                CloseQuickMenu();
                return QuickMenuAction::Close;
            }
        } else if (occupied) {
            if (DeleteSaveState(slot)) {
                VideoCore::PostOverlayToast("Deleted " + SaveStateSlotName(slot));
            }
            RefreshSaveStates();
        }
        RebuildRows();
        Repaint();
        return QuickMenuAction::None;
    }

    if (row.item == Item::AmiiboFile && nav.confirm) {
        if (LoadAmiibo(s_amiibos[static_cast<std::size_t>(row.index)].path)) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        Repaint();
        return QuickMenuAction::None;
    }
    if (row.item == Item::RemoveAmiibo && nav.confirm) {
        RemoveAmiibo();
        Repaint();
        return QuickMenuAction::None;
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
