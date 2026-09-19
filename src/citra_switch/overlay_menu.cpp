// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "citra_switch/camera/still_image_camera.h"
#include "citra_switch/config.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/overlay_menu.h"
#include "citra_switch/settings_menu.h"
#include "core/cheats/cheat_base.h"
#include "core/cheats/cheats.h"
#include "core/core.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/service/cam/cam.h"
#include "core/hle/service/nfc/nfc.h"
#include "core/hle/service/sm/sm.h"
#include "core/loader/loader.h"
#include "core/savestate.h"
#include "video_core/overlay.h"

namespace SwitchFrontend {

namespace {

// The kind of row. Setting rows index into the current page's row list, cheat rows into the
// engine's cheat list, save state rows into the slots, and section rows into Page.
enum class Item {
    Setting,
    Section,
    Separator,
    SaveHere,
    LoadHere,
    Cheat,
    CheatsEmpty,
    SaveStateSlot,
    AmiiboFile,
    AmiibosEmpty,
    RemoveAmiibo,
    CameraTargetRow,
    CameraImage,
    CameraImagesEmpty,
    ClearCamera,
    Resume,
    ExitGame,
};

struct Row {
    Item item;
    int index = -1;
};

// The overlay opens on Home, which carries the things worth reaching for mid-game and links out
// to the rest. The settings pages are numbered to match QuickSection so one cast converts them.
enum class Page {
    Home,
    Display,
    Graphics,
    Stereo,
    Audio,
    Input,
    System,
    States,
    Cheats,
    Amiibo,
    Camera,
    Count,
};

constexpr int NumPages = static_cast<int>(Page::Count);

// Every page but Home, which L/R cycles through once you are inside one.
constexpr int NumSections = NumPages - 1;

static_assert(static_cast<int>(Page::Display) == static_cast<int>(QuickSection::Display) &&
                  static_cast<int>(Page::System) == static_cast<int>(QuickSection::System),
              "the settings pages have to line up with QuickSection");

bool IsSettingsPage(Page page) {
    return page >= Page::Display && page <= Page::System;
}

const char* PageName(Page page) {
    switch (page) {
    case Page::Home:
        return "Quick Menu";
    case Page::States:
        return "Save States";
    case Page::Cheats:
        return "Cheats";
    case Page::Amiibo:
        return "Amiibo";
    case Page::Camera:
        return "Camera";
    default:
        return QuickSectionName(static_cast<QuickSection>(page));
    }
}

constexpr int kMaxVisibleRows = 10;

// Cheats past this many spill onto further sub-pages.
constexpr int kCheatsPerPage = 8;

// Save state slots past this many spill onto further sub-pages.
constexpr int kSlotsPerPage = 8;

constexpr int kAmiibosPerPage = 7;

// One row is spent on the target selector and one on the clear action.
constexpr int kCameraImagesPerPage = 6;

std::atomic<bool> s_open{false};
std::atomic<bool> s_pause_in_menu{false};
int s_page = 0;
int s_selected = 0;
int s_cheat_page = 0;
int s_state_page = 0;
int s_amiibo_page = 0;
int s_camera_page = 0;
std::vector<Row> s_rows;
std::vector<SettingsRow> s_settings;
int s_scroll = 0;
int s_home_slot = 0;
std::vector<FileEntry> s_amiibos;
std::vector<FileEntry> s_camera_images;
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

void RefreshCameraImages() {
    s_camera_images = ListCameraImages();
}

int CameraPageCount() {
    const int count = static_cast<int>(s_camera_images.size());
    return count <= 0 ? 1 : (count + kCameraImagesPerPage - 1) / kCameraImagesPerPage;
}

// Makes a running game pick the cameras up again.
void ReloadGuestCameras() {
    auto& system = Core::System::GetInstance();
    if (!system.IsPoweredOn()) {
        return;
    }
    if (auto cam = Service::CAM::GetModule(system)) {
        cam->ReloadCameraDevices();
    }
}

void SelectCameraImage(const std::string& path) {
    // Decoding here rather than on the capture thread means a broken file is reported to the
    // player.
    const std::string error = Camera::StillImage::Preload(path);
    if (!error.empty()) {
        VideoCore::PostOverlayToast("Cannot use that image: " + error);
        return;
    }
    SetCameraImage(path, GetCameraTarget());
    ReloadGuestCameras();
    VideoCore::PostOverlayToast("Cameras now show this image");
}

void ClearCameraImage() {
    if (GetCameraImage().empty()) {
        VideoCore::PostOverlayToast("The cameras are already blank");
        return;
    }
    SetCameraImage("", GetCameraTarget());
    ReloadGuestCameras();
    VideoCore::PostOverlayToast("Cameras cleared");
}

void StepCameraTarget(int dir) {
    const int count = NumCameraTargets;
    const int next = (static_cast<int>(GetCameraTarget()) + dir + count) % count;
    SetCameraImage(GetCameraImage(), static_cast<CameraTarget>(next));
    ReloadGuestCameras();
}

Page CurrentPage() {
    return static_cast<Page>(std::clamp(s_page, 0, NumPages - 1));
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
void BuildHomeRows() {
    s_rows.push_back({Item::Resume});
    s_rows.push_back({Item::ExitGame});
    s_rows.push_back({Item::SaveHere});
    s_rows.push_back({Item::LoadHere});
    s_rows.push_back({Item::Separator, 0});
    for (int page = static_cast<int>(Page::Display); page <= static_cast<int>(Page::System);
         ++page) {
        s_rows.push_back({Item::Section, page});
    }
    s_rows.push_back({Item::Separator, 1});
    for (int page = static_cast<int>(Page::States); page < NumPages; ++page) {
        s_rows.push_back({Item::Section, page});
    }
}

void RebuildRows() {
    s_rows.clear();
    s_settings.clear();
    const Page page = CurrentPage();
    if (page == Page::Home) {
        BuildHomeRows();
    } else if (IsSettingsPage(page)) {
        s_settings = BuildQuickRows(static_cast<QuickSection>(page));
        for (int i = 0; i < static_cast<int>(s_settings.size()); ++i) {
            s_rows.push_back({Item::Setting, i});
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
    } else if (page == Page::Amiibo) {
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
    } else {
        s_rows.push_back({Item::CameraTargetRow});
        s_camera_page = std::clamp(s_camera_page, 0, CameraPageCount() - 1);
        const int first = s_camera_page * kCameraImagesPerPage;
        const int last =
            std::min(static_cast<int>(s_camera_images.size()), first + kCameraImagesPerPage);
        for (int i = first; i < last; ++i) {
            s_rows.push_back({Item::CameraImage, i});
        }
        if (s_camera_images.empty()) {
            s_rows.push_back({Item::CameraImagesEmpty});
        }
        s_rows.push_back({Item::ClearCamera});
    }
    s_selected = std::clamp(s_selected, 0, static_cast<int>(s_rows.size()) - 1);
    if (s_rows.empty()) {
        return;
    }
    if (s_rows[static_cast<std::size_t>(s_selected)].item == Item::Separator) {
        ++s_selected;
    }
    const int max_scroll = std::max(0, static_cast<int>(s_rows.size()) - kMaxVisibleRows);
    s_scroll = std::clamp(s_scroll, std::max(0, s_selected - kMaxVisibleRows + 1),
                          std::min(s_selected, max_scroll));
}

// Rows that only respond to A.
bool IsAction(const Row& row) {
    switch (row.item) {
    case Item::Setting:
    case Item::Cheat:
    case Item::CameraTargetRow:
    case Item::SaveHere:
    case Item::LoadHere:
        return false;
    default:
        return true;
    }
}

const char* SeparatorLabel(int index) {
    return index == 0 ? "Settings" : "Game";
}

std::string SlotSummary(int slot) {
    const std::string& status = s_slot_status[static_cast<std::size_t>(slot)];
    return SaveStateSlotName(static_cast<unsigned int>(slot)) +
           (status.empty() ? " - empty" : " - " + status);
}

std::string Label(const Row& row) {
    switch (row.item) {
    case Item::Setting:
        return s_settings[static_cast<std::size_t>(row.index)].label;
    case Item::Section:
        return std::string{PageName(static_cast<Page>(row.index))} + "  >";
    case Item::Separator:
        return SeparatorLabel(row.index);
    case Item::SaveHere:
        return "Save State";
    case Item::LoadHere:
        return "Load State";
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
    case Item::CameraTargetRow:
        return "Feed image to";
    case Item::CameraImage:
        return s_camera_images[static_cast<std::size_t>(row.index)].name;
    case Item::CameraImagesEmpty:
        return "No .png/.jpg files in camera/";
    case Item::ClearCamera:
        return "Show nothing";
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
    case Item::SaveHere:
    case Item::LoadHere:
        return SlotSummary(s_home_slot);
    case Item::Cheat:
        return CheatEnabled(row.index) ? "On" : "Off";
    case Item::SaveStateSlot: {
        const std::string& status = s_slot_status[static_cast<std::size_t>(row.index)];
        return status.empty() ? "Empty" : status;
    }
    case Item::CameraTargetRow:
        return CameraTargetName(GetCameraTarget());
    case Item::CameraImage:
        return s_camera_images[static_cast<std::size_t>(row.index)].path == GetCameraImage()
                   ? "In use"
                   : "";
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
    case Item::SaveHere:
    case Item::LoadHere:
        s_home_slot = (s_home_slot + dir + static_cast<int>(Core::SaveStateSlotCount)) %
                      static_cast<int>(Core::SaveStateSlotCount);
        break;
    case Item::Cheat:
        ToggleCheat(row.index);
        break;
    case Item::CameraTargetRow:
        StepCameraTarget(dir);
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

std::string ListSuffix(int page, int pages) {
    return pages > 1 ? "  List " + std::to_string(page + 1) + "/" + std::to_string(pages)
                     : std::string{};
}

const char* HintFor(Page page) {
    switch (page) {
    case Page::Home:
        return "A Select   +/- Close";
    case Page::States:
        return "A Load   X Save   Y Delete   ZL/ZR List   B Back";
    case Page::Cheats:
        return "A Toggle   ZL/ZR List   B Back";
    case Page::Amiibo:
        return "A Load/Remove   ZL/ZR List   B Back";
    case Page::Camera:
        return "A Select   ZL/ZR List   B Back";
    default:
        return "A Change   L/R Section   B Back";
    }
}

void Repaint() {
    const Page page = CurrentPage();
    VideoCore::OverlayMenuState state;
    state.visible = s_open.load(std::memory_order_relaxed);
    state.title = page == Page::Home ? std::string{PageName(page)}
                                     : std::string{"Quick Menu > "} + PageName(page);
    switch (page) {
    case Page::Cheats:
        state.title += ListSuffix(s_cheat_page, CheatPageCount());
        break;
    case Page::States:
        state.title += ListSuffix(s_state_page, StatePageCount());
        break;
    case Page::Amiibo:
        state.title += ListSuffix(s_amiibo_page, AmiiboPageCount());
        break;
    case Page::Camera:
        state.title += ListSuffix(s_camera_page, CameraPageCount());
        break;
    default:
        break;
    }
    state.hint = HintFor(page);

    const int count = static_cast<int>(s_rows.size());
    const int visible = std::min(count, kMaxVisibleRows);
    const int first = std::clamp(s_scroll, 0, std::max(0, count - visible));
    if (count > visible) {
        state.hint += "   " + std::to_string(first + 1) + "-" + std::to_string(first + visible) +
                      " of " + std::to_string(count);
    }

    state.selected = s_selected - first;
    state.items.reserve(static_cast<std::size_t>(visible));
    for (int i = first; i < first + visible; ++i) {
        const Row& row = s_rows[static_cast<std::size_t>(i)];
        std::string value = Value(row);
        const bool no_value = value.empty();
        state.items.push_back(
            {Label(row), std::move(value), no_value, row.item == Item::Separator});
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
    s_page = static_cast<int>(Page::Home);
    s_selected = 0;
    s_scroll = 0;
    s_cheat_page = 0;
    s_state_page = 0;
    s_amiibo_page = 0;
    s_camera_page = 0;
    RefreshSaveStates();
    RefreshAmiibos();
    RefreshCameraImages();
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

namespace {

void EnterPage(Page page) {
    s_page = static_cast<int>(page);
    s_selected = 0;
    s_scroll = 0;
    switch (page) {
    case Page::States:
        RefreshSaveStates();
        break;
    case Page::Amiibo:
        RefreshAmiibos();
        break;
    case Page::Camera:
        RefreshCameraImages();
        break;
    default:
        break;
    }
    RebuildRows();
}

void ClampScroll() {
    const int count = static_cast<int>(s_rows.size());
    const int visible = std::min(count, kMaxVisibleRows);
    s_scroll = std::clamp(s_scroll, s_selected - visible + 1, s_selected);
    s_scroll = std::clamp(s_scroll, 0, std::max(0, count - visible));
}

void MoveSelection(int dir) {
    const int count = static_cast<int>(s_rows.size());
    if (count == 0) {
        return;
    }
    for (int step = 0; step < count; ++step) {
        s_selected = (s_selected + dir + count) % count;
        if (s_rows[static_cast<std::size_t>(s_selected)].item != Item::Separator) {
            break;
        }
    }
    ClampScroll();
}

bool StepSubList(const QuickMenuNav& nav) {
    if (!nav.page_prev && !nav.page_next) {
        return false;
    }
    const int dir = nav.page_next ? 1 : -1;
    int* target = nullptr;
    int pages = 0;
    switch (CurrentPage()) {
    case Page::Cheats:
        target = &s_cheat_page;
        pages = CheatPageCount();
        break;
    case Page::States:
        target = &s_state_page;
        pages = StatePageCount();
        break;
    case Page::Amiibo:
        target = &s_amiibo_page;
        pages = AmiiboPageCount();
        break;
    case Page::Camera:
        target = &s_camera_page;
        pages = CameraPageCount();
        break;
    default:
        return false;
    }
    *target = (*target + dir + pages) % pages;
    s_selected = 0;
    s_scroll = 0;
    RebuildRows();
    return true;
}

} // namespace

QuickMenuAction UpdateQuickMenu(const QuickMenuNav& nav) {
    if (!IsQuickMenuOpen()) {
        return QuickMenuAction::None;
    }

    if (nav.cancel) {
        if (CurrentPage() != Page::Home) {
            const int came_from = s_page;
            EnterPage(Page::Home);
            for (int i = 0; i < static_cast<int>(s_rows.size()); ++i) {
                const Row& row = s_rows[static_cast<std::size_t>(i)];
                if (row.item == Item::Section && row.index == came_from) {
                    s_selected = i;
                    break;
                }
            }
            ClampScroll();
            Repaint();
            return QuickMenuAction::None;
        }
        CloseQuickMenu();
        return QuickMenuAction::Close;
    }

    bool changed = false;

    if (nav.tab_prev != nav.tab_next && CurrentPage() != Page::Home) {
        const int current = s_page - static_cast<int>(Page::Display);
        const int next = (current + (nav.tab_next ? 1 : -1) + NumSections) % NumSections;
        EnterPage(static_cast<Page>(next + static_cast<int>(Page::Display)));
        changed = true;
    }

    changed |= StepSubList(nav);

    if (nav.up) {
        MoveSelection(-1);
        changed = true;
    }
    if (nav.down) {
        MoveSelection(+1);
        changed = true;
    }

    if (s_rows.empty()) {
        if (changed) {
            Repaint();
        }
        return QuickMenuAction::None;
    }

    const Row row = s_rows[static_cast<std::size_t>(s_selected)];
    if (nav.left && !IsAction(row)) {
        Adjust(row, -1);
        changed = true;
    }
    if (nav.right && !IsAction(row)) {
        Adjust(row, 1);
        changed = true;
    }

    if (row.item == Item::Section && nav.confirm) {
        EnterPage(static_cast<Page>(row.index));
        Repaint();
        return QuickMenuAction::None;
    }

    if ((row.item == Item::SaveHere || row.item == Item::LoadHere) && nav.confirm) {
        const auto slot = static_cast<unsigned int>(s_home_slot);
        if (row.item == Item::SaveHere) {
            if (RequestSaveState(slot)) {
                CloseQuickMenu();
                return QuickMenuAction::Close;
            }
        } else if (s_slot_status[slot].empty()) {
            VideoCore::PostOverlayToast(SaveStateSlotName(slot) + " is empty");
        } else if (RequestLoadState(slot)) {
            CloseQuickMenu();
            return QuickMenuAction::Close;
        }
        Repaint();
        return QuickMenuAction::None;
    }

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

    if (row.item == Item::CameraImage && nav.confirm) {
        SelectCameraImage(s_camera_images[static_cast<std::size_t>(row.index)].path);
        RebuildRows();
        Repaint();
        return QuickMenuAction::None;
    }
    if (row.item == Item::ClearCamera && nav.confirm) {
        ClearCameraImage();
        RebuildRows();
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
