// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace SwitchFrontend {

enum class UpdateChannel {
    Stable,
    Prerelease,
};

struct UpdateRelease {
    std::string tag;
    std::string name;
    std::string download_url;
    std::string sha256;
    std::uint64_t size{};
    bool prerelease{};
};

enum class UpdateCheckStatus {
    Available,
    UpToDate,
    Error,
};

struct UpdateCheckResult {
    UpdateCheckStatus status{UpdateCheckStatus::Error};
    UpdateRelease release;
    std::string error;
};

struct UpdateInstallResult {
    bool success{};
    std::string error;
    std::string backup_path;
};

using UpdateProgressCallback =
    std::function<void(std::uint64_t downloaded, std::uint64_t total)>;

// Version embedded in both the NACP and the updater's comparisons.
const char* CurrentVersion();

// Records argv[0] so the updater replaces the exact NRO the user launched.
void SetUpdaterExecutablePath(const std::string& path);
const std::string& GetUpdaterExecutablePath();

// Normalise strings against qualifiers
int CompareReleaseVersions(const std::string& lhs, const std::string& rhs);

// Queries the public GitHub Releases API for new verisons.
UpdateCheckResult CheckForUpdate(UpdateChannel channel,
                                 const std::atomic<bool>* cancel = nullptr);

// Downloads and verifies release into a temporary file beside executable_path, then replaces the
// running NRO.
UpdateInstallResult InstallUpdate(const UpdateRelease& release,
                                  const std::string& executable_path,
                                  UpdateProgressCallback progress);

} // namespace SwitchFrontend
