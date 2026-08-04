// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "citra_switch/menu_data.h"

// Import/export of guest save data, laid out the same way Checkpoint does.
namespace SwitchFrontend {

enum class SaveKind {
    SaveData, // The title's own SD save.
    ExtData,  // The title's extdata.
};

struct SaveBackup {
    std::string name;
    std::string path;
    std::uint64_t size{};
    std::uint64_t files{};
    // Whether an export left the format info beside this backup.
    bool has_format_info{};
};

enum class SaveResult {
    Success,
    NoSaveData,   // Nothing on the emulated card to export.
    NotFormatted, // The restore target has no format info and the backup carries none.
    BackupEmpty,
    CopyFailed,
    NoExtData, // The title declares no extdata ID.
    Unknown,   // The title ID couldn't be read.
};

const char* SaveResultText(SaveResult result);
const char* SaveKindName(SaveKind kind);

// The extdata ID `game` declares in its exheader.
std::uint64_t GetExtDataId(const GameEntry& game);

// Where backups of this title live.
std::string GetBackupDir(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id);

// Whether the emulated card holds save data of this kind for `game`.
bool HasSaveData(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id);

// Existing backups, newest first.
std::vector<SaveBackup> ListBackups(const GameEntry& game, SaveKind kind,
                                    std::uint64_t extdata_id);

// A folder name for a new backup.
std::string DefaultBackupName();

// Strips what FAT won't take in a file name.
std::string SanitizeBackupName(const std::string& name);

SaveResult ExportSave(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id,
                      const std::string& name);

SaveResult ImportSave(const GameEntry& game, SaveKind kind, std::uint64_t extdata_id,
                      const SaveBackup& backup);

bool DeleteBackup(const SaveBackup& backup);

} // namespace SwitchFrontend
