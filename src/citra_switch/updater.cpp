// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf (palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "citra_switch/updater.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <curl/curl.h>
#include <cryptopp/sha.h>
#include <json.hpp>
#include "citra_switch/config.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "common/string_util.h"

#ifndef DEKOPON_VERSION
#define DEKOPON_VERSION "0.0.0"
#endif

namespace SwitchFrontend {
namespace {

constexpr std::string_view kReleasesApi =
    "https://api.github.com/repos/PalindromicBreadLoaf/dekopon/releases?per_page=20";
constexpr std::string_view kUserAgent = "Dekopon-Updater/" DEKOPON_VERSION;
constexpr long kConnectTimeoutSeconds = 15;
constexpr long kRequestTimeoutSeconds = 30;
constexpr std::uint32_t kNroMagic = 0x304F524E;
constexpr std::string_view kNotesFileName = "release_notes.txt";
std::string s_executable_path;

struct VersionPart {
    bool numeric{};
    std::uint64_t number{};
    std::string text;
};

struct ParsedVersion {
    std::array<std::uint64_t, 3> core{};
    std::vector<VersionPart> prerelease;
    bool valid{};
};

std::string LowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string NormalizeVersion(std::string value) {
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(),
                             [](unsigned char c) { return !std::isspace(c); }));
    value.erase(std::find_if(value.rbegin(), value.rend(),
                             [](unsigned char c) { return !std::isspace(c); })
                    .base(),
                value.end());
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
        value.erase(value.begin());
    }
    return LowerAscii(std::move(value));
}

bool ParseUnsigned(std::string_view text, std::uint64_t& value) {
    if (text.empty()) {
        return false;
    }
    value = 0;
    for (const unsigned char c : text) {
        if (!std::isdigit(c)) {
            return false;
        }
        const unsigned digit = c - '0';
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    return true;
}

ParsedVersion ParseVersion(const std::string& raw) {
    const std::string version = NormalizeVersion(raw);
    ParsedVersion parsed;
    const std::size_t dash = version.find('-');
    const std::string_view core{version.data(), dash == std::string::npos ? version.size() : dash};

    std::size_t begin = 0;
    for (std::size_t i = 0; i < parsed.core.size(); ++i) {
        const std::size_t end = core.find('.', begin);
        const std::string_view component = core.substr(begin, end - begin);
        if (!ParseUnsigned(component, parsed.core[i])) {
            return parsed;
        }
        if (i + 1 < parsed.core.size()) {
            if (end == std::string_view::npos) {
                return parsed;
            }
            begin = end + 1;
        } else if (end != std::string_view::npos) {
            return parsed;
        }
    }

    if (dash != std::string::npos) {
        const std::string_view suffix{version.data() + dash + 1, version.size() - dash - 1};
        std::size_t pos = 0;
        while (pos < suffix.size()) {
            while (pos < suffix.size() &&
                   (suffix[pos] == '.' || suffix[pos] == '-' || suffix[pos] == '_')) {
                ++pos;
            }
            if (pos == suffix.size()) {
                break;
            }
            const bool numeric = std::isdigit(static_cast<unsigned char>(suffix[pos])) != 0;
            const std::size_t token_begin = pos;
            while (pos < suffix.size() && suffix[pos] != '.' && suffix[pos] != '-' &&
                   suffix[pos] != '_' &&
                   (std::isdigit(static_cast<unsigned char>(suffix[pos])) != 0) == numeric) {
                ++pos;
            }
            VersionPart part{.numeric = numeric};
            const std::string_view token = suffix.substr(token_begin, pos - token_begin);
            if (numeric) {
                if (!ParseUnsigned(token, part.number)) {
                    return {};
                }
            } else {
                part.text.assign(token);
            }
            parsed.prerelease.push_back(std::move(part));
        }
        if (parsed.prerelease.empty()) {
            return {};
        }
    }
    parsed.valid = true;
    return parsed;
}

int CompareParsed(const ParsedVersion& lhs, const ParsedVersion& rhs) {
    if (lhs.core != rhs.core) {
        return lhs.core < rhs.core ? -1 : 1;
    }
    if (lhs.prerelease.empty() != rhs.prerelease.empty()) {
        return lhs.prerelease.empty() ? 1 : -1;
    }
    for (std::size_t i = 0; i < std::min(lhs.prerelease.size(), rhs.prerelease.size()); ++i) {
        const VersionPart& a = lhs.prerelease[i];
        const VersionPart& b = rhs.prerelease[i];
        if (a.numeric != b.numeric) {
            return a.numeric ? -1 : 1;
        }
        if (a.numeric && a.number != b.number) {
            return a.number < b.number ? -1 : 1;
        }
        if (!a.numeric && a.text != b.text) {
            return a.text < b.text ? -1 : 1;
        }
    }
    if (lhs.prerelease.size() == rhs.prerelease.size()) {
        return 0;
    }
    return lhs.prerelease.size() < rhs.prerelease.size() ? -1 : 1;
}

class CurlHandle {
public:
    CurlHandle() : handle(curl_easy_init()) {}
    ~CurlHandle() {
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;

    explicit operator bool() const {
        return handle != nullptr;
    }
    CURL* Get() const {
        return handle;
    }

private:
    CURL* handle{};
};

class CurlHeaders {
public:
    ~CurlHeaders() {
        if (headers) {
            curl_slist_free_all(headers);
        }
    }
    bool Add(const char* header) {
        curl_slist* next = curl_slist_append(headers, header);
        if (!next) {
            return false;
        }
        headers = next;
        return true;
    }
    curl_slist* Get() const {
        return headers;
    }

private:
    curl_slist* headers{};
};

void ConfigureCurl(CURL* curl, std::string_view url) {
    curl_easy_setopt(curl, CURLOPT_URL, std::string{url}.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, std::string{kUserAgent}.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, kConnectTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, kRequestTimeoutSeconds);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
}

std::size_t WriteString(char* data, std::size_t size, std::size_t count, void* userdata) {
    const std::size_t bytes = size * count;
    static_cast<std::string*>(userdata)->append(data, bytes);
    return bytes;
}

int AbortWhenCancelled(void* userdata, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<const std::atomic<bool>*>(userdata)->load() ? 1 : 0;
}

std::optional<std::string> GetReleaseJson(const std::atomic<bool>* cancel, std::string& error) {
    CurlHandle curl;
    CurlHeaders headers;
    if (!curl || !headers.Add("Accept: application/vnd.github+json") ||
        !headers.Add("X-GitHub-Api-Version: 2022-11-28")) {
        error = "Couldn't initialize the update request.";
        return std::nullopt;
    }
    std::string body;
    ConfigureCurl(curl.Get(), kReleasesApi);
    curl_easy_setopt(curl.Get(), CURLOPT_HTTPHEADER, headers.Get());
    curl_easy_setopt(curl.Get(), CURLOPT_WRITEFUNCTION, WriteString);
    curl_easy_setopt(curl.Get(), CURLOPT_WRITEDATA, &body);
    if (cancel) {
        curl_easy_setopt(curl.Get(), CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl.Get(), CURLOPT_XFERINFOFUNCTION, AbortWhenCancelled);
        curl_easy_setopt(curl.Get(), CURLOPT_XFERINFODATA, cancel);
    }

    const CURLcode result = curl_easy_perform(curl.Get());
    if (result != CURLE_OK) {
        error = std::string{"GitHub request failed: "} + curl_easy_strerror(result);
        return std::nullopt;
    }
    long status = 0;
    curl_easy_getinfo(curl.Get(), CURLINFO_RESPONSE_CODE, &status);
    if (status != 200) {
        error = "GitHub returned HTTP " + std::to_string(status) + '.';
        return std::nullopt;
    }
    return body;
}

// A malformed release is simply skipped.
std::optional<UpdateRelease> ParseRelease(const nlohmann::json& release) try {
    if (!release.is_object() || release.value("draft", true)) {
        return std::nullopt;
    }
    UpdateRelease out;
    out.tag = release.value("tag_name", "");
    out.name = release.value("name", out.tag);
    out.notes = release.value("body", "");
    out.prerelease = release.value("prerelease", false);
    if (out.tag.empty()) {
        return std::nullopt;
    }
    const auto assets_it = release.find("assets");
    if (assets_it == release.end() || !assets_it->is_array()) {
        return std::nullopt;
    }
    for (const auto& asset : *assets_it) {
        if (!asset.is_object() || asset.value("name", "") != "dekopon.nro") {
            continue;
        }
        out.download_url = asset.value("browser_download_url", "");
        out.size = asset.value("size", std::uint64_t{});
        std::string digest = asset.value("digest", "");
        constexpr std::string_view prefix = "sha256:";
        if (digest.rfind(prefix, 0) == 0) {
            out.sha256 = LowerAscii(digest.substr(prefix.size()));
        }
        break;
    }
    if (out.download_url.empty() || out.size == 0 || out.sha256.size() != 64) {
        return std::nullopt;
    }
    return out;
} catch (const nlohmann::json::exception& e) {
    LOG_WARNING(Frontend, "Skipping malformed release entry: {}", e.what());
    return std::nullopt;
}

std::string HexDigest(const std::array<CryptoPP::byte, CryptoPP::SHA256::DIGESTSIZE>& digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.resize(digest.size() * 2);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        result[i * 2] = hex[digest[i] >> 4];
        result[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    return result;
}

struct DownloadState {
    std::FILE* file{};
    CryptoPP::SHA256 hash;
    std::uint64_t written{};
    UpdateProgressCallback progress;
};

std::size_t WriteDownload(char* data, std::size_t size, std::size_t count, void* userdata) {
    DownloadState& state = *static_cast<DownloadState*>(userdata);
    const std::size_t bytes = size * count;
    const std::size_t written = std::fwrite(data, 1, bytes, state.file);
    if (written != 0) {
        state.hash.Update(reinterpret_cast<const CryptoPP::byte*>(data), written);
        state.written += written;
    }
    return written;
}

int ReportProgress(void* userdata, curl_off_t total, curl_off_t current, curl_off_t, curl_off_t) {
    DownloadState& state = *static_cast<DownloadState*>(userdata);
    if (state.progress) {
        state.progress(static_cast<std::uint64_t>(std::max<curl_off_t>(0, current)),
                       static_cast<std::uint64_t>(std::max<curl_off_t>(0, total)));
    }
    return 0;
}

bool IsNro(const std::string& path) {
    std::FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) {
        return false;
    }
    std::array<unsigned char, 0x10> start{};
    std::uint32_t magic{};
    const bool read = std::fread(start.data(), start.size(), 1, file) == 1 &&
                      std::fread(&magic, sizeof(magic), 1, file) == 1;
    std::fclose(file);
    return read && magic == kNroMagic;
}

std::string ErrnoMessage(const char* action) {
    return std::string{action} + ": " + std::strerror(errno);
}

std::string NotesPath() {
    return GetActiveUserDir() + std::string{kNotesFileName};
}

} // namespace

const char* CurrentVersion() {
    return DEKOPON_VERSION;
}

void SetUpdaterExecutablePath(const std::string& path) {
    s_executable_path = path;
}

const std::string& GetUpdaterExecutablePath() {
    return s_executable_path;
}

int CompareReleaseVersions(const std::string& lhs, const std::string& rhs) {
    const ParsedVersion a = ParseVersion(lhs);
    const ParsedVersion b = ParseVersion(rhs);
    if (!a.valid || !b.valid) {
        const std::string normalized_a = NormalizeVersion(lhs);
        const std::string normalized_b = NormalizeVersion(rhs);
        if (normalized_a == normalized_b) {
            return 0;
        }
        return normalized_a < normalized_b ? -1 : 1;
    }
    return CompareParsed(a, b);
}

UpdateCheckResult CheckForUpdate(UpdateChannel channel, const std::atomic<bool>* cancel) {
    UpdateCheckResult result;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        result.error = "Couldn't initialize network services.";
        return result;
    }

    std::string request_error;
    const std::optional<std::string> body = GetReleaseJson(cancel, request_error);
    if (!body) {
        curl_global_cleanup();
        result.error = std::move(request_error);
        LOG_WARNING(Frontend, "Update check failed: {}", result.error);
        return result;
    }

    const nlohmann::json releases = nlohmann::json::parse(*body, nullptr, false);
    if (!releases.is_array()) {
        curl_global_cleanup();
        result.error = "GitHub returned invalid release data.";
        LOG_WARNING(Frontend, "Update check failed: {}", result.error);
        return result;
    }

    std::optional<UpdateRelease> newest;
    for (const auto& item : releases) {
        std::optional<UpdateRelease> release = ParseRelease(item);
        if (!release) {
            continue;
        }
        if (result.current_notes.empty() &&
            CompareReleaseVersions(release->tag, CurrentVersion()) == 0) {
            result.current_notes = release->notes;
        }
        if (release->prerelease && channel == UpdateChannel::Stable) {
            continue;
        }
        if (!newest || CompareReleaseVersions(release->tag, newest->tag) > 0) {
            newest = std::move(release);
        }
    }
    curl_global_cleanup();

    if (!newest) {
        result.error = "No compatible dekopon.nro release was found.";
        return result;
    }
    if (CompareReleaseVersions(newest->tag, CurrentVersion()) <= 0) {
        result.status = UpdateCheckStatus::UpToDate;
        result.release = std::move(*newest);
        return result;
    }
    result.status = UpdateCheckStatus::Available;
    result.release = std::move(*newest);
    return result;
}

UpdateInstallResult InstallUpdate(const UpdateRelease& release,
                                  const std::string& executable_path,
                                  UpdateProgressCallback progress) {
    UpdateInstallResult result;
    if (executable_path.empty() || executable_path.find(".nro") == std::string::npos) {
        result.error = "The path of the running NRO could not be determined.";
        return result;
    }

    std::FILE* current = std::fopen(executable_path.c_str(), "rb");
    if (!current) {
        result.error = "The running NRO could not be opened at " + executable_path;
        return result;
    }
    std::fclose(current);

    const std::string temporary = executable_path + ".update";
    const std::string backup = executable_path + ".backup";
    std::remove(temporary.c_str());

    std::FILE* output = std::fopen(temporary.c_str(), "wb");
    if (!output) {
        result.error = ErrnoMessage("Couldn't create the update file");
        return result;
    }

    DownloadState state{.file = output, .progress = std::move(progress)};
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        std::fclose(output);
        std::remove(temporary.c_str());
        result.error = "Couldn't initialize network services.";
        return result;
    }
    CURLcode download = CURLE_FAILED_INIT;
    {
        CurlHandle curl;
        if (curl) {
            ConfigureCurl(curl.Get(), release.download_url);
            curl_easy_setopt(curl.Get(), CURLOPT_TIMEOUT, 0L);
            curl_easy_setopt(curl.Get(), CURLOPT_LOW_SPEED_LIMIT, 1024L);
            curl_easy_setopt(curl.Get(), CURLOPT_LOW_SPEED_TIME, 30L);
            curl_easy_setopt(curl.Get(), CURLOPT_WRITEFUNCTION, WriteDownload);
            curl_easy_setopt(curl.Get(), CURLOPT_WRITEDATA, &state);
            curl_easy_setopt(curl.Get(), CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl.Get(), CURLOPT_XFERINFOFUNCTION, ReportProgress);
            curl_easy_setopt(curl.Get(), CURLOPT_XFERINFODATA, &state);
            download = curl_easy_perform(curl.Get());
        }
    }
    const bool flushed = std::fflush(output) == 0;
    const bool closed = std::fclose(output) == 0;
    curl_global_cleanup();
    if (download != CURLE_OK || !flushed || !closed) {
        std::remove(temporary.c_str());
        result.error = download != CURLE_OK
                           ? std::string{"Download failed: "} + curl_easy_strerror(download)
                           : "Writing the update to the SD card failed.";
        return result;
    }

    std::array<CryptoPP::byte, CryptoPP::SHA256::DIGESTSIZE> digest{};
    state.hash.Final(digest.data());
    if (state.written != release.size) {
        std::remove(temporary.c_str());
        result.error = "The downloaded update has the wrong size.";
        return result;
    }
    if (HexDigest(digest) != LowerAscii(release.sha256)) {
        std::remove(temporary.c_str());
        result.error = "The downloaded update failed its SHA-256 check.";
        return result;
    }
    if (!IsNro(temporary)) {
        std::remove(temporary.c_str());
        result.error = "The downloaded update is not a valid NRO.";
        return result;
    }

    // Keep the verified old NRO as a recovery file,
    // and restore it immediately if installing the new file fails.
    std::remove(backup.c_str());
    if (std::rename(executable_path.c_str(), backup.c_str()) != 0) {
        std::remove(temporary.c_str());
        result.error = ErrnoMessage("Couldn't back up the current NRO");
        return result;
    }
    if (std::rename(temporary.c_str(), executable_path.c_str()) != 0) {
        const std::string install_error = ErrnoMessage("Couldn't install the new NRO");
        if (std::rename(backup.c_str(), executable_path.c_str()) != 0) {
            result.error = install_error + " The backup is at " + backup + '.';
        } else {
            result.error = install_error + " The previous NRO was restored.";
        }
        return result;
    }

    result.success = true;
    result.backup_path = backup;
    LOG_INFO(Frontend, "Updated Dekopon {} -> {}, backup at {}", CurrentVersion(), release.tag,
             backup);
    return result;
}

CachedReleaseNotes LoadCachedReleaseNotes() {
    CachedReleaseNotes cached;
    std::string contents;
    if (FileUtil::ReadFileToString(true, NotesPath(), contents) == 0) {
        return cached;
    }
    const std::size_t newline = contents.find('\n');
    if (newline == std::string::npos) {
        return cached;
    }
    cached.tag = Common::StripSpaces(contents.substr(0, newline));
    cached.notes = contents.substr(newline + 1);
    return cached;
}

void CacheReleaseNotes(const std::string& tag, const std::string& notes) {
    if (tag.empty() || GetActiveUserDir().empty()) {
        return;
    }
    // Every launch's update check lands here, so the SD write only happens on a real change.
    const CachedReleaseNotes cached = LoadCachedReleaseNotes();
    if (cached.tag == tag && cached.notes == notes) {
        return;
    }
    FileUtil::WriteStringToFile(true, NotesPath(), tag + '\n' + notes);
}

} // namespace SwitchFrontend
