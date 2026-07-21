// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <optional>

#include <fmt/format.h>

#include "citra_switch/config.h"
#include "citra_switch/input.h"
#include "citra_switch/menu_data.h"
#include "citra_switch/overlay_menu.h"
#include "common/file_util.h"
#include "common/string_util.h"
#include "common/settings.h"
#include "common/zstd_compression.h"
#include "core/core.h"
#include "core/file_sys/cia_container.h"
#include "core/file_sys/title_metadata.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"
#include "core/loader/smdh.h"

namespace SwitchFrontend {

namespace {

constexpr std::uint64_t kTidHighMask = 0xFFFFFFFF00000000ULL;
constexpr std::uint64_t kTidHighApplication = 0x0004000000000000ULL;
constexpr std::uint64_t kTidHighDemo = 0x0004000200000000ULL;
constexpr std::uint64_t kTidHighUpdate = 0x0004000E00000000ULL;
constexpr std::uint64_t kTidHighDlc = 0x0004008C00000000ULL;

// Don't scan updates/dlcs into the library window.
constexpr std::array<std::uint64_t, 2> kLibraryTidHighs{kTidHighApplication, kTidHighDemo};

// Decode game icons
std::uint32_t Rgb565ToRgba8888(std::uint16_t c) {
    const std::uint32_t r5 = (c >> 11) & 0x1F;
    const std::uint32_t g6 = (c >> 5) & 0x3F;
    const std::uint32_t b5 = c & 0x1F;
    const std::uint32_t r = (r5 << 3) | (r5 >> 2);
    const std::uint32_t g = (g6 << 2) | (g6 >> 4);
    const std::uint32_t b = (b5 << 3) | (b5 >> 2);
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

// Trims a UTF-16 title into a single clean UTF-8 line.
std::string CleanTitle(const std::array<char16_t, 0x80>& raw) {
    std::u16string u16{raw.data(),
                       std::char_traits<char16_t>::length(raw.data()) > raw.size()
                           ? raw.size()
                           : std::char_traits<char16_t>::length(raw.data())};
    std::string out = Common::UTF16ToUTF8(u16);
    for (char& ch : out) {
        if (ch == '\n' || ch == '\r' || ch == '\t') {
            ch = ' ';
        }
    }
    // Collapse the runs of spaces the newline replacement leaves.
    std::string collapsed;
    collapsed.reserve(out.size());
    bool prev_space = false;
    for (char ch : out) {
        const bool space = ch == ' ';
        if (space && prev_space) {
            continue;
        }
        collapsed.push_back(ch);
        prev_space = space;
    }
    while (!collapsed.empty() && collapsed.back() == ' ') {
        collapsed.pop_back();
    }
    return collapsed;
}

void FillFromSmdh(GameEntry& entry, const Loader::SMDH& smdh) {
    using Language = Loader::SMDH::TitleLanguage;
    const auto title = CleanTitle(smdh.GetLongTitle(Language::English));
    if (!title.empty()) {
        entry.title = title;
    }
    const auto& pub = smdh.titles[static_cast<std::size_t>(Language::English)].publisher;
    entry.publisher = Common::UTF16ToUTF8(std::u16string{pub.data(),
        std::char_traits<char16_t>::length(pub.data())});

    const std::vector<u16> icon = smdh.GetIcon(true);
    if (icon.size() == 48 * 48) {
        entry.icon_size = 48;
        entry.icon.resize(icon.size());
        std::transform(icon.begin(), icon.end(), entry.icon.begin(), Rgb565ToRgba8888);
    }
}

// Reads one candidate file into a GameEntry, or returns false if it isn't a title.
// `fallback_title` names the entry when there is no SMDH to read a long title out of.
bool TryLoad(const std::string& path, const std::string& fallback_title, GameEntry& entry) {
    std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(path);
    if (!loader) {
        return false;
    }
    bool is_executable = false;
    if (loader->IsExecutable(is_executable) != Loader::ResultStatus::Success || !is_executable) {
        return false;
    }

    entry.path = path;
    entry.title = fallback_title;
    entry.file_type = Loader::GetFileTypeString(loader->GetFileType(), loader->IsFileCompressed());
    loader->ReadProgramId(entry.program_id);

    std::vector<u8> smdh_buffer;
    const Loader::ResultStatus icon_result = loader->ReadIcon(smdh_buffer);
    if (icon_result == Loader::ResultStatus::ErrorEncrypted) {
        entry.encrypted = true;
    } else if (icon_result == Loader::ResultStatus::Success &&
               Loader::IsValidSMDH(smdh_buffer)) {
        Loader::SMDH smdh;
        std::memcpy(&smdh, smdh_buffer.data(), sizeof(Loader::SMDH));
        FillFromSmdh(entry, smdh);
    }
    return true;
}

void ScanDirectory(const std::string& directory, std::vector<GameEntry>& out, int depth,
                   bool recursive) {
    if (depth > 4) {
        return;
    }
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out, depth, recursive](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                if (recursive) {
                    ScanDirectory(path + '/', out, depth + 1, recursive);
                }
                return true;
            }
            GameEntry entry;
            if (TryLoad(path, std::string{FileUtil::GetFilename(path)}, entry)) {
                out.push_back(std::move(entry));
            }
            return true;
        });
}

// Parses a title tree folder name ("00053f00") into the low word of a title ID.
bool ParseTidLow(const std::string& name, std::uint32_t& out) {
    if (name.size() != 8) {
        return false;
    }
    std::uint32_t value = 0;
    for (const char c : name) {
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            return false;
        }
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    out = value;
    return true;
}

// Adds the titles installed under the emulated SD card's title tree.
void ScanInstalled(std::vector<GameEntry>& out) {
    const std::string title_root =
        Service::AM::GetMediaTitlePath(Service::FS::MediaType::SDMC);
    for (const std::uint64_t high : kLibraryTidHighs) {
        const std::string dir = fmt::format("{}{:08x}/", title_root, high >> 32);
        FileUtil::ForeachDirectoryEntry(
            nullptr, dir,
            [&out, high](u64*, const std::string& parent, const std::string& virtual_name) {
                if (!FileUtil::IsDirectory(parent + virtual_name)) {
                    return true;
                }
                std::uint32_t low = 0;
                if (!ParseTidLow(virtual_name, low)) {
                    return true;
                }
                const std::uint64_t tid = high | low;
                // Resolves the boot content through the title's TMD, so it picks the same .app
                // the loader would boot.
                const std::string content =
                    Service::AM::GetTitleContentPath(Service::FS::MediaType::SDMC, tid);
                if (content.empty() || !FileUtil::Exists(content)) {
                    return true;
                }
                GameEntry entry;
                if (!TryLoad(content, fmt::format("{:016X}", tid), entry)) {
                    return true;
                }
                entry.installed = true;
                if (entry.program_id == 0) {
                    entry.program_id = tid;
                }
                out.push_back(std::move(entry));
                return true;
            });
    }
}

// Reads a CIA's header and TMD, which both sit ahead of the content.
bool ReadCiaEntry(const std::string& path, CiaEntry& entry) {
    std::unique_ptr<FileUtil::IOFile> file = std::make_unique<FileUtil::IOFile>(path, "rb");
    if (!file->IsOpen()) {
        return false;
    }
    entry.compressed = FileUtil::Z3DSReadIOFile::GetUnderlyingFileMagic(file.get()) != std::nullopt;
    if (entry.compressed) {
        file = std::make_unique<FileUtil::Z3DSReadIOFile>(std::move(file));
    }

    std::vector<u8> header(FileSys::CIA_HEADER_SIZE);
    if (file->ReadBytes(header.data(), header.size()) != header.size()) {
        return false;
    }
    FileSys::CIAContainer container;
    if (container.LoadHeader(header) != Loader::ResultStatus::Success) {
        return false;
    }

    std::vector<u8> tmd(container.GetTitleMetadataSize());
    if (file->ReadAtBytes(tmd.data(), tmd.size(), container.GetTitleMetadataOffset()) !=
            tmd.size() ||
        container.LoadTitleMetadata(tmd) != Loader::ResultStatus::Success) {
        return false;
    }

    entry.program_id = container.GetTitleMetadata().GetTitleID();
    entry.kind = ClassifyTitle(entry.program_id);
    entry.version = container.GetTitleMetadata().GetTitleVersion();
    return true;
}

struct InstalledTmd {
    std::uint16_t version{};
    int content_count{};
};

// Reads the TMD of `tid`, or nothing if that title isn't installed.
std::optional<InstalledTmd> ReadInstalledTmd(std::uint64_t tid) {
    FileSys::TitleMetadata tmd;
    const std::string path =
        Service::AM::GetTitleMetadataPath(Service::AM::GetTitleMediaType(tid), tid);
    if (tmd.Load(path) != Loader::ResultStatus::Success) {
        return std::nullopt;
    }
    return InstalledTmd{tmd.GetTitleVersion(), static_cast<int>(tmd.GetContentCount())};
}

} // namespace

TitleKind ClassifyTitle(std::uint64_t program_id) {
    switch (program_id & kTidHighMask) {
    case kTidHighApplication:
        return TitleKind::Application;
    case kTidHighDemo:
        return TitleKind::Demo;
    case kTidHighUpdate:
        return TitleKind::Update;
    case kTidHighDlc:
        return TitleKind::AddOnContent;
    default:
        break;
    }
    // Everything the emulated NAND holds.
    return Service::AM::GetTitleMediaType(program_id) == Service::FS::MediaType::NAND
               ? TitleKind::System
               : TitleKind::Other;
}

const char* TitleKindName(TitleKind kind) {
    switch (kind) {
    case TitleKind::Application:
        return "Game";
    case TitleKind::Demo:
        return "Demo";
    case TitleKind::Update:
        return "Update";
    case TitleKind::AddOnContent:
        return "DLC";
    case TitleKind::System:
        return "System";
    default:
        return "Other";
    }
}

std::string FormatTitleVersion(std::uint16_t version) {
    return fmt::format("v{}.{}.{} ({})", (version >> 10) & 0x3F, (version >> 4) & 0x3F,
                       version & 0xF, version);
}

TitleDetails GetTitleDetails(const GameEntry& entry) {
    TitleDetails details;
    details.program_id = entry.program_id;
    details.kind = ClassifyTitle(entry.program_id);
    if (entry.program_id == 0) {
        return details;
    }

    if (entry.installed) {
        if (const std::optional<InstalledTmd> tmd = ReadInstalledTmd(entry.program_id)) {
            details.has_base_version = true;
            details.base_version = tmd->version;
        }
    }
    if (details.kind != TitleKind::Application && details.kind != TitleKind::Demo) {
        return details;
    }

    // The loader keys the update off the base title's low word.
    const std::uint64_t low = entry.program_id & 0xFFFFFFFFULL;
    if (const std::optional<InstalledTmd> tmd = ReadInstalledTmd(kTidHighUpdate | low)) {
        details.has_update = true;
        details.update_version = tmd->version;
    }
    if (const std::optional<InstalledTmd> tmd = ReadInstalledTmd(kTidHighDlc | low)) {
        details.has_dlc = true;
        details.dlc_contents = tmd->content_count;
    }
    return details;
}

bool GetInstalledVersion(std::uint64_t program_id, std::uint16_t& version) {
    const std::optional<InstalledTmd> tmd = ReadInstalledTmd(program_id);
    if (!tmd) {
        return false;
    }
    version = tmd->version;
    return true;
}

std::vector<CiaEntry> ListCiaFiles(const std::string& directory) {
    std::vector<CiaEntry> out;
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                return true;
            }
            std::string extension;
            Common::SplitPath(path, nullptr, nullptr, &extension);
            extension = Common::ToLower(extension);
            if (extension != ".cia" && extension != ".zcia") {
                return true;
            }
            CiaEntry entry;
            entry.name = virtual_name;
            entry.path = path;
            entry.size = FileUtil::GetSize(path);
            entry.readable = ReadCiaEntry(path, entry);
            out.push_back(std::move(entry));
            return true;
        });
    std::sort(out.begin(), out.end(), [](const CiaEntry& a, const CiaEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    });
    return out;
}

InstallResult InstallCia(const std::string& path,
                         const std::function<void(std::size_t, std::size_t)>& progress) {
    const Service::AM::InstallStatus status = Service::AM::InstallCIA(
        path, [&progress](std::size_t written, std::size_t total) {
            if (progress) {
                progress(written, total);
            }
        });
    switch (status) {
    case Service::AM::InstallStatus::Success:
        return InstallResult::Success;
    case Service::AM::InstallStatus::ErrorFileNotFound:
        return InstallResult::FileNotFound;
    case Service::AM::InstallStatus::ErrorFailedToOpenFile:
        return InstallResult::FailedToOpen;
    case Service::AM::InstallStatus::ErrorAborted:
        return InstallResult::Aborted;
    case Service::AM::InstallStatus::ErrorEncrypted:
        return InstallResult::Encrypted;
    default:
        return InstallResult::Invalid;
    }
}

const char* InstallResultText(InstallResult result) {
    switch (result) {
    case InstallResult::Success:
        return "Installed";
    case InstallResult::FileNotFound:
        return "File not found";
    case InstallResult::FailedToOpen:
        return "Couldn't open the file";
    case InstallResult::Aborted:
        return "Install aborted";
    case InstallResult::Encrypted:
        return "CIA is encrypted. Please decrypt it or add aes_keys.txt";
    default:
        return "Not a valid CIA";
    }
}

std::vector<GameEntry> ScanGames() {
    const SwitchPaths& paths = GetPaths();
    FileUtil::CreateFullPath(paths.roms_dir);

    std::vector<GameEntry> games;
    ScanDirectory(paths.roms_dir, games, 0, paths.scan_recursive);
    ScanInstalled(games);

    std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
        return Common::ToLower(a.title) < Common::ToLower(b.title);
    });
    return games;
}

std::vector<DirEntry> ListSubdirectories(const std::string& directory) {
    std::vector<DirEntry> out;
    FileUtil::ForeachDirectoryEntry(
        nullptr, directory,
        [&out](u64*, const std::string& dir, const std::string& virtual_name) {
            const std::string path = dir + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                out.push_back(DirEntry{virtual_name, path + '/'});
            }
            return true;
        });
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        return Common::ToLower(a.name) < Common::ToLower(b.name);
    });
    return out;
}

std::string ParentDirectory(const std::string& directory) {
    if (directory.size() <= 1) {
        return "";
    }
    // Skip the trailing '/' so the search lands on the separator above it.
    const std::size_t sep = directory.find_last_of('/', directory.size() - 2);
    if (sep == std::string::npos) {
        return "";
    }
    return directory.substr(0, sep + 1);
}

bool EnsureDirectory(const std::string& directory) {
    return FileUtil::CreateFullPath(directory) && FileUtil::IsDirectory(directory);
}

MenuSettings GetMenuSettings() {
    const auto& v = Settings::values;
    return MenuSettings{
        .resolution_factor = static_cast<int>(v.resolution_factor.GetValue()),
        .use_vsync = v.use_vsync.GetValue(),
        .async_shader_compilation = v.async_shader_compilation.GetValue(),
        .use_disk_shader_cache = v.use_disk_shader_cache.GetValue(),
        .use_hw_shader = v.use_hw_shader.GetValue(),
        .texture_filter = static_cast<int>(v.texture_filter.GetValue()),
        .filter_mode = v.filter_mode.GetValue(),
        .use_integer_scaling = v.use_integer_scaling.GetValue(),
        .show_fps = v.show_fps.GetValue(),
        .disable_right_eye_render = v.disable_right_eye_render.GetValue(),
        .preload_textures = v.preload_textures.GetValue(),
        .dump_textures = v.dump_textures.GetValue(),
        .cpu_clock_percentage = static_cast<int>(v.cpu_clock_percentage.GetValue()),
        .is_new_3ds = v.is_new_3ds.GetValue(),
        .use_cpu_jit = v.use_cpu_jit.GetValue(),
        .region_value = static_cast<int>(v.region_value.GetValue()),
        .language = static_cast<int>(
            Service::CFG::GetModule(Core::System::GetInstance())->GetSystemLanguage()),
        .graphics_api = static_cast<int>(Settings::GetWorkingGraphicsAPI()),
        .pointer_source = static_cast<int>(GetPointerSource()),
        .gyro_sensitivity_x = GetGyroSensitivityX(),
        .gyro_sensitivity_y = GetGyroSensitivityY(),
        .layout_cycle_mask = GetLayoutCycleMask(),
        .pause_in_quick_menu = IsPauseInQuickMenu(),
    };
}

void SetMenuSettings(const MenuSettings& s) {
    auto& v = Settings::values;
    v.resolution_factor = static_cast<u32>(std::clamp(s.resolution_factor, 0, 10));
    v.use_vsync = s.use_vsync;
    v.async_shader_compilation = s.async_shader_compilation;
    v.use_disk_shader_cache = s.use_disk_shader_cache;
    v.use_hw_shader = s.use_hw_shader;
    v.texture_filter = static_cast<Settings::TextureFilter>(std::clamp(s.texture_filter, 0, 5));
    v.filter_mode = s.filter_mode;
    v.use_integer_scaling = s.use_integer_scaling;
    v.show_fps = s.show_fps;
    v.disable_right_eye_render = s.disable_right_eye_render;
    v.preload_textures = s.preload_textures;
    v.dump_textures = s.dump_textures;
    v.cpu_clock_percentage = std::clamp(s.cpu_clock_percentage, 5, 400);
    v.is_new_3ds = s.is_new_3ds;
    v.use_cpu_jit = s.use_cpu_jit;
    v.region_value = std::clamp(s.region_value, -1, 6);
    SetPointerSource(static_cast<PointerSource>(
        std::clamp(s.pointer_source, 0, NumPointerSources - 1)));
    SetGyroSensitivity(s.gyro_sensitivity_x, s.gyro_sensitivity_y);
    SetLayoutCycleMask(s.layout_cycle_mask);
    SetPauseInQuickMenu(s.pause_in_quick_menu);
    SaveConfig();

    // The 3DS system language lives in the CFG NAND savegame rather than config.ini,
    // so it's written straight through the CFG module.
    auto cfg = Service::CFG::GetModule(Core::System::GetInstance());
    const auto language =
        static_cast<Service::CFG::SystemLanguage>(std::clamp(s.language, 0, 11));
    if (cfg->GetSystemLanguage() != language) {
        cfg->SetSystemLanguage(language);
        cfg->UpdateConfigNANDSavegame();
    }
}

} // namespace SwitchFrontend
