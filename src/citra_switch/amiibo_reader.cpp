// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>
#include <mutex>
#include <switch.h>

#include "citra_switch/amiibo_reader.h"

namespace SwitchFrontend {

namespace {

static_assert(sizeof(NfpData) == 0x298);
static_assert(offsetof(NfpData, tag_magic) == NfpOffset::TagMagic);
static_assert(offsetof(NfpData, tag_write_counter) == NfpOffset::TagWriteCounter);
static_assert(offsetof(NfpData, crc32_1) == NfpOffset::SettingsCrc);
static_assert(offsetof(NfpData, last_write_date) == NfpOffset::LastWriteDate);
static_assert(offsetof(NfpData, write_counter) == NfpOffset::ApplicationWriteCounter);
static_assert(offsetof(NfpData, version) == NfpOffset::Version);
static_assert(offsetof(NfpData, mii_v3) == NfpOffset::MiiV3);
static_assert(offsetof(NfpData, mii_store_data_extension) == NfpOffset::MiiStoreDataExtension);
static_assert(offsetof(NfpData, first_write_date) == NfpOffset::FirstWriteDate);
static_assert(offsetof(NfpData, amiibo_name) == NfpOffset::AmiiboName);
static_assert(offsetof(NfpData, font_region) == NfpOffset::FontRegion);
static_assert(offsetof(NfpData, unknown1) == NfpOffset::Unknown1);
static_assert(offsetof(NfpData, crc32_2) == NfpOffset::RegisterInfoCrc);
static_assert(offsetof(NfpData, unknown2) == NfpOffset::Unknown2);
static_assert(offsetof(NfpData, application_id) == NfpOffset::ApplicationId);
static_assert(offsetof(NfpData, access_id) == NfpOffset::AccessId);
static_assert(offsetof(NfpData, settings_crc32_change_counter) == NfpOffset::CrcCounter);
static_assert(offsetof(NfpData, flags) == NfpOffset::Flags);
static_assert(offsetof(NfpData, application_id_byte) == NfpOffset::ApplicationIdByte);
static_assert(offsetof(NfpData, application_area) == NfpOffset::ApplicationArea);
static_assert(sizeof(NfpData::application_area) == NfpOffset::ApplicationAreaSize);

static_assert(offsetof(NfpData, mii_v3_crc16) - offsetof(NfpData, mii_v3) == 0x5E);

constexpr s32 kMaxDevices = 8;

std::mutex g_mutex;

bool g_session_open = false;
bool g_scanning = false;
bool g_mounted = false;
bool g_app_area_open = false;
std::uint32_t g_app_area_id = 0;
s32 g_device_count = 0;
NfcDeviceHandle g_devices[kMaxDevices]{};
NfcDeviceHandle g_tag_device{};

std::string ResultText(const char* what, Result rc) {
    char buffer[96];
    std::snprintf(buffer, sizeof(buffer), "%s failed (%04d-%04d)", what, 2000 + R_MODULE(rc),
                  R_DESCRIPTION(rc));
    return buffer;
}

void CloseSession() {
    if (!g_session_open) {
        return;
    }
    if (g_mounted) {
        nfpUnmount(&g_tag_device);
        g_mounted = false;
    }
    g_app_area_open = false;
    for (s32 i = 0; i < g_device_count; ++i) {
        nfpStopDetection(&g_devices[i]);
    }
    nfpExit();
    g_session_open = false;
    g_scanning = false;
    g_device_count = 0;
}

} // Anonymous namespace

bool IsAmiiboReaderAvailable() {
    const std::scoped_lock lock{g_mutex};
    if (g_session_open) {
        return true;
    }
    if (R_FAILED(nfpInitialize(NfpServiceType_Debug))) {
        return false;
    }
    nfpExit();
    return true;
}

bool StartAmiiboScan(std::string& error) {
    const std::scoped_lock lock{g_mutex};
    if (g_scanning) {
        return true;
    }
    CloseSession();

    Result rc = nfpInitialize(NfpServiceType_Debug);
    if (R_FAILED(rc)) {
        error = ResultText("nfp:dbg", rc);
        return false;
    }
    g_session_open = true;

    rc = nfpListDevices(&g_device_count, g_devices, kMaxDevices);
    if (R_FAILED(rc) || g_device_count <= 0) {
        bool nfc_enabled = false;
        if (R_SUCCEEDED(nfcInitialize(NfcServiceType_User))) {
            nfcIsNfcEnabled(&nfc_enabled);
            nfcExit();
        }
        error = nfc_enabled ? "No NFC reader is connected"
                            : "NFC is switched off (flight mode disables it)";
        CloseSession();
        return false;
    }

    for (s32 i = 0; i < g_device_count; ++i) {
        nfpStartDetection(&g_devices[i]);
    }
    g_scanning = true;
    return true;
}

void StopAmiiboScan() {
    const std::scoped_lock lock{g_mutex};
    if (!g_scanning || g_mounted) {
        return;
    }
    CloseSession();
}

bool IsAmiiboScanning() {
    const std::scoped_lock lock{g_mutex};
    return g_scanning && !g_mounted;
}

namespace {

bool ReadMountedTag(const NfcDeviceHandle& device, AmiiboCapture& out, std::string& error) {
    NfpData data{};
    Result rc = nfpGetAll(&device, &data);
    if (R_FAILED(rc)) {
        error = ResultText("GetAll", rc);
        return false;
    }

    NfpTagInfo tag_info{};
    rc = nfpGetTagInfo(&device, &tag_info);
    if (R_FAILED(rc)) {
        error = ResultText("GetTagInfo", rc);
        return false;
    }

    NfpModelInfo model_info{};
    rc = nfpGetModelInfo(&device, &model_info);
    if (R_FAILED(rc)) {
        error = ResultText("GetModelInfo", rc);
        return false;
    }

    out = {};
    std::memcpy(out.nfp_data.data(), &data, sizeof(data));
    const std::size_t uid_length =
        tag_info.uid.uid_length < out.uid.size() ? tag_info.uid.uid_length : out.uid.size();
    std::memcpy(out.uid.data(), tag_info.uid.uid, uid_length);
    out.character_id = model_info.game_character_id;
    out.character_variant = model_info.character_variant;
    out.amiibo_type = model_info.nfp_type;
    out.model_number = model_info.numbering_id;
    out.series = model_info.series_id;

    NfpRegisterInfo register_info{};
    if (R_SUCCEEDED(nfpGetRegisterInfo(&device, &register_info))) {
        out.name = register_info.amiibo_name;
    }
    return true;
}

} // Anonymous namespace

bool PollAmiiboScan(AmiiboCapture& out, std::string& error) {
    const std::scoped_lock lock{g_mutex};
    if (!g_scanning || g_mounted) {
        return false;
    }

    NfcDeviceHandle device{};
    bool found = false;
    s32 found_index = -1;
    for (s32 i = 0; i < g_device_count && !found; ++i) {
        NfpDeviceState state = NfpDeviceState_Initialized;
        if (R_FAILED(nfpGetDeviceState(&g_devices[i], &state))) {
            continue;
        }
        if (state == NfpDeviceState_TagFound) {
            device = g_devices[i];
            found_index = i;
            found = true;
        }
    }
    if (!found) {
        return false;
    }

    for (s32 i = 0; i < g_device_count; ++i) {
        if (i != found_index) {
            nfpStopDetection(&g_devices[i]);
        }
    }

    const Result rc = nfpMount(&device, NfpDeviceType_Amiibo, NfpMountTarget_All);
    if (R_FAILED(rc)) {
        error = ResultText("Mount", rc);
        nfpStopDetection(&device);
        CloseSession();
        return false;
    }

    if (!ReadMountedTag(device, out, error)) {
        nfpUnmount(&device);
        nfpStopDetection(&device);
        CloseSession();
        return false;
    }

    g_tag_device = device;
    g_mounted = true;
    g_app_area_open = false;
    return true;
}

bool RecaptureAmiibo(AmiiboCapture& out, std::string& error) {
    const std::scoped_lock lock{g_mutex};
    if (!g_mounted) {
        error = "The amiibo is no longer on the reader";
        return false;
    }
    return ReadMountedTag(g_tag_device, out, error);
}

void ResumeAmiiboScanning() {
    const std::scoped_lock lock{g_mutex};
    if (!g_session_open) {
        return;
    }
    if (g_mounted) {
        nfpUnmount(&g_tag_device);
        g_mounted = false;
    }
    g_app_area_open = false;
    for (s32 i = 0; i < g_device_count; ++i) {
        nfpStartDetection(&g_devices[i]);
    }
    g_scanning = true;
}

bool IsAmiiboSessionOpen() {
    const std::scoped_lock lock{g_mutex};
    return g_session_open;
}

bool IsAmiiboOnReader() {
    const std::scoped_lock lock{g_mutex};
    if (!g_mounted) {
        return false;
    }
    NfpDeviceState state = NfpDeviceState_Initialized;
    if (R_FAILED(nfpGetDeviceState(&g_tag_device, &state))) {
        return false;
    }
    return state == NfpDeviceState_TagMounted || state == NfpDeviceState_TagFound;
}

AmiiboWriteResult WriteAmiiboApplicationArea(std::uint32_t access_id, const std::uint8_t* data,
                                             std::size_t size, std::string& error) {
    const std::scoped_lock lock{g_mutex};
    if (!g_mounted) {
        error = "The amiibo is no longer on the reader";
        return AmiiboWriteResult::Failed;
    }

    if (!g_app_area_open || g_app_area_id != access_id) {
        const Result open_rc = nfpOpenApplicationArea(&g_tag_device, access_id);
        if (R_FAILED(open_rc)) {
            error = ResultText("OpenApplicationArea", open_rc);
            return AmiiboWriteResult::NeedsOverwrite;
        }
        g_app_area_open = true;
        g_app_area_id = access_id;
    }

    Result rc = nfpSetApplicationArea(&g_tag_device, data, size);
    if (R_FAILED(rc)) {
        error = ResultText("SetApplicationArea", rc);
        g_app_area_open = false;
        return AmiiboWriteResult::Failed;
    }

    rc = nfpFlush(&g_tag_device);
    if (R_FAILED(rc)) {
        error = ResultText("Flush", rc);
        return AmiiboWriteResult::Failed;
    }
    return AmiiboWriteResult::Ok;
}

bool OverwriteAmiiboApplicationArea(std::uint32_t access_id, const std::uint8_t* data,
                                    std::size_t size, std::string& error) {
    const std::scoped_lock lock{g_mutex};
    if (!g_mounted) {
        error = "The amiibo is no longer on the reader";
        return false;
    }

    NfpAdminInfo admin{};
    const bool area_exists = R_SUCCEEDED(nfpGetAdminInfo(&g_tag_device, &admin)) &&
                             (admin.flags & NfpAmiiboFlag_ApplicationAreaExists) != 0;

    const Result rc =
        area_exists ? nfpRecreateApplicationArea(&g_tag_device, access_id, data, size)
                    : nfpCreateApplicationArea(&g_tag_device, access_id, data, size);
    if (R_FAILED(rc)) {
        error = ResultText(area_exists ? "RecreateApplicationArea" : "CreateApplicationArea", rc);
        return false;
    }

    const Result flush_rc = nfpFlush(&g_tag_device);
    if (R_FAILED(flush_rc)) {
        error = ResultText("Flush", flush_rc);
        return false;
    }

    g_app_area_open = true;
    g_app_area_id = access_id;
    return true;
}

std::uint64_t GetAmiiboAreaOwner() {
    const std::scoped_lock lock{g_mutex};
    if (!g_mounted) {
        return 0;
    }
    NfpAdminInfo admin{};
    if (R_FAILED(nfpGetAdminInfo(&g_tag_device, &admin))) {
        return 0;
    }
    if ((admin.flags & NfpAmiiboFlag_ApplicationAreaExists) == 0) {
        return 0;
    }
    return admin.application_id;
}

void ReleaseAmiibo() {
    const std::scoped_lock lock{g_mutex};
    CloseSession();
}

} // namespace SwitchFrontend
