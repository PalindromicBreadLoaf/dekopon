// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <ctime>
#include <memory>

#include <fmt/format.h>

#include "citra_switch/save_manager.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/file_sys/archive_extsavedata.h"
#include "core/file_sys/archive_source_sd_savedata.h"
#include "core/loader/loader.h"

namespace SwitchFrontend {
namespace {

// Checkpoint's normal layout for interoperability.
constexpr const char* kCheckpointRoot = "sdmc:/3ds/Checkpoint/";

std::string SdmcDir() {
    return FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir);
}

// The emulator-side directory the guest sees as the archive root.
std::string GuestSaveDir(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id) {
    if (kind == SaveKind::SaveData) {
        return FileSys::ArchiveSource_SDSaveData::GetSaveDataPathFor(SdmcDir(), game.program_id);
    }
    return FileSys::GetExtDataPathFromId(SdmcDir(), extdata_id) + "user/";
}

// Where the ArchiveFormatInfo blob for that archive lives.
std::string GuestFormatInfoPath(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id) {
    if (kind == SaveKind::SaveData) {
        const std::string dir =
            FileSys::ArchiveSource_SDSaveData::GetSaveDataPathFor(SdmcDir(), game.program_id);
        return dir.substr(0, dir.size() - 1) + ".metadata";
    }
    return FileSys::GetExtDataPathFromId(SdmcDir(), extdata_id) + "metadata";
}

std::string BackupFormatInfoPath(const std::string& backup_dir, const std::string& name) {
    return backup_dir + name + ".metadata";
}

bool IsDotEntry(const std::string& name) {
    return name == "." || name == "..";
}

bool CopyTree(const std::string& src, const std::string& dst) {
    if (!FileUtil::CreateFullPath(dst)) {
        return false;
    }
    bool ok = true;
    FileUtil::ForeachDirectoryEntry(
        nullptr, src,
        [&ok, &dst](u64*, const std::string& dir, const std::string& virtual_name) {
            if (IsDotEntry(virtual_name)) {
                return true;
            }
            const std::string source = dir + virtual_name;
            const std::string dest = dst + virtual_name;
            if (FileUtil::IsDirectory(source)) {
                ok = CopyTree(source + '/', dest + '/') && ok;
            } else if (!FileUtil::Copy(source, dest)) {
                LOG_ERROR(Frontend, "Failed to copy {} to {}", source, dest);
                ok = false;
            }
            return ok;
        });
    return ok;
}

void MeasureTree(const std::string& dir, std::uint64_t& size, std::uint64_t& files) {
    FileUtil::ForeachDirectoryEntry(
        nullptr, dir,
        [&size, &files](u64*, const std::string& parent, const std::string& virtual_name) {
            if (IsDotEntry(virtual_name)) {
                return true;
            }
            const std::string path = parent + virtual_name;
            if (FileUtil::IsDirectory(path)) {
                MeasureTree(path + '/', size, files);
            } else {
                size += FileUtil::GetSize(path);
                ++files;
            }
            return true;
        });
}

bool DirectoryHasEntries(const std::string& dir) {
    if (!FileUtil::IsDirectory(dir)) {
        return false;
    }
    bool found = false;
    FileUtil::ForeachDirectoryEntry(
        nullptr, dir, [&found](u64*, const std::string&, const std::string& virtual_name) {
            if (!IsDotEntry(virtual_name)) {
                found = true;
            }
            return !found;
        });
    return found;
}

u32 UniqueId(std::uint64_t program_id) {
    return static_cast<u32>((program_id >> 8) & 0xFFFFF);
}

} // namespace

const char* SaveResultText(SaveResult result) {
    switch (result) {
    case SaveResult::Success:
        return "Done";
    case SaveResult::NoSaveData:
        return "This title has no save data yet";
    case SaveResult::NotFormatted:
        return "Launch the game once to create its save, then restore";
    case SaveResult::BackupEmpty:
        return "That backup is empty";
    case SaveResult::CopyFailed:
        return "Copy failed, the card may be full or write-protected";
    case SaveResult::NoExtData:
        return "This title has no extdata";
    case SaveResult::Unknown:
    default:
        return "Could not read the title ID";
    }
}

const char* SaveKindName(SaveKind kind) {
    return kind == SaveKind::SaveData ? "Save data" : "Extdata";
}

std::uint64_t GetExtDataId(const GameEntry& game) {
    std::unique_ptr<Loader::AppLoader> loader = Loader::GetLoader(game.path);
    if (!loader) {
        return 0;
    }
    u64 extdata_id = 0;
    if (loader->ReadExtdataId(extdata_id) != Loader::ResultStatus::Success) {
        return 0;
    }
    return extdata_id;
}

std::string SanitizeBackupName(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        // FAT rejects these.
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
            c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20) {
            out += '_';
        } else {
            out += c;
        }
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    return out;
}

std::string GetBackupDir(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id) {
    const u32 unique_id =
        kind == SaveKind::SaveData ? UniqueId(game.program_id) : static_cast<u32>(extdata_id);
    const std::string title = SanitizeBackupName(game.title);
    return fmt::format("{}{}/0x{:05X} {}/", kCheckpointRoot,
                       kind == SaveKind::SaveData ? "saves" : "extdata", unique_id, title);
}

bool HasSaveData(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id) {
    if (game.program_id == 0 || (kind == SaveKind::ExtData && extdata_id == 0)) {
        return false;
    }
    return DirectoryHasEntries(GuestSaveDir(game, kind, extdata_id));
}

std::vector<SaveBackup> ListBackups(const GameEntry& game, SaveKind kind,
                                    std::uint64_t extdata_id) {
    std::vector<SaveBackup> out;
    if (game.program_id == 0 || (kind == SaveKind::ExtData && extdata_id == 0)) {
        return out;
    }
    const std::string dir = GetBackupDir(game, kind, extdata_id);
    if (!FileUtil::IsDirectory(dir)) {
        return out;
    }
    FileUtil::ForeachDirectoryEntry(
        nullptr, dir,
        [&out, &dir](u64*, const std::string& parent, const std::string& virtual_name) {
            if (IsDotEntry(virtual_name)) {
                return true;
            }
            const std::string path = parent + virtual_name;
            if (!FileUtil::IsDirectory(path)) {
                return true;
            }
            SaveBackup backup;
            backup.name = virtual_name;
            backup.path = path + '/';
            backup.has_format_info = FileUtil::Exists(BackupFormatInfoPath(dir, virtual_name));
            MeasureTree(backup.path, backup.size, backup.files);
            out.push_back(std::move(backup));
            return true;
        });
    // Timestamped names sort newest first this way.
    std::sort(out.begin(), out.end(),
              [](const SaveBackup& a, const SaveBackup& b) { return a.name > b.name; });
    return out;
}

std::string DefaultBackupName() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return fmt::format("{:04}{:02}{:02}-{:02}{:02}{:02}", local.tm_year + 1900, local.tm_mon + 1,
                       local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
}

SaveResult ExportSave(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id,
                      const std::string& name) {
    if (game.program_id == 0) {
        return SaveResult::Unknown;
    }
    if (kind == SaveKind::ExtData && extdata_id == 0) {
        return SaveResult::NoExtData;
    }
    const std::string src = GuestSaveDir(game, kind, extdata_id);
    if (!DirectoryHasEntries(src)) {
        return SaveResult::NoSaveData;
    }

    const std::string backup_dir = GetBackupDir(game, kind, extdata_id);
    const std::string dest = backup_dir + name + '/';
    if (FileUtil::Exists(dest)) {
        FileUtil::DeleteDirRecursively(dest);
    }
    if (!FileUtil::CreateFullPath(dest)) {
        return SaveResult::CopyFailed;
    }
    if (!CopyTree(src, dest)) {
        return SaveResult::CopyFailed;
    }

    const std::string format_info = GuestFormatInfoPath(game, kind, extdata_id);
    if (FileUtil::Exists(format_info)) {
        FileUtil::Copy(format_info, BackupFormatInfoPath(backup_dir, name));
    }
    return SaveResult::Success;
}

SaveResult ImportSave(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id,
                      const SaveBackup& backup) {
    if (game.program_id == 0) {
        return SaveResult::Unknown;
    }
    if (kind == SaveKind::ExtData && extdata_id == 0) {
        return SaveResult::NoExtData;
    }
    if (!DirectoryHasEntries(backup.path)) {
        return SaveResult::BackupEmpty;
    }

    const std::string format_info = GuestFormatInfoPath(game, kind, extdata_id);
    if (!FileUtil::Exists(format_info)) {
        const std::string backup_dir = GetBackupDir(game, kind, extdata_id);
        const std::string carried = BackupFormatInfoPath(backup_dir, backup.name);
        if (!FileUtil::Exists(carried)) {
            return SaveResult::NotFormatted;
        }
        if (!FileUtil::CreateFullPath(format_info.substr(0, format_info.rfind('/') + 1)) ||
            !FileUtil::Copy(carried, format_info)) {
            return SaveResult::CopyFailed;
        }
    }

    const std::string dest = GuestSaveDir(game, kind, extdata_id);
    // Restoring over a save that holds more files than the backup would otherwise leave the
    // leftovers behind for the game to read.
    FileUtil::DeleteDirRecursively(dest);
    if (!FileUtil::CreateFullPath(dest)) {
        return SaveResult::CopyFailed;
    }
    if (!CopyTree(backup.path, dest)) {
        return SaveResult::CopyFailed;
    }

    if (kind == SaveKind::ExtData) {
        FileUtil::CreateFullPath(FileSys::GetExtDataPathFromId(SdmcDir(), extdata_id) + "boss/");
    }
    return SaveResult::Success;
}

bool DeleteBackup(const SaveBackup& backup) {
    const std::string parent = backup.path.substr(0, backup.path.size() - 1);
    const std::string dir = parent.substr(0, parent.rfind('/') + 1);
    FileUtil::Delete(BackupFormatInfoPath(dir, backup.name));
    return FileUtil::DeleteDirRecursively(backup.path);
}

} // namespace SwitchFrontend
