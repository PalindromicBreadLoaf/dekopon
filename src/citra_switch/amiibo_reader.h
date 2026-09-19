// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace SwitchFrontend {

struct AmiiboCapture {
    std::array<std::uint8_t, 0x298> nfp_data{}; // libnx NfpData
    std::array<std::uint8_t, 7> uid{};
    std::uint16_t character_id{};
    std::uint8_t character_variant{};
    std::uint8_t amiibo_type{};
    std::uint16_t model_number{};
    std::uint8_t series{};
    std::string name;
};

namespace NfpOffset {
inline constexpr std::size_t TagMagic = 0x00;
inline constexpr std::size_t TagWriteCounter = 0x02;
inline constexpr std::size_t SettingsCrc = 0x04;
inline constexpr std::size_t LastWriteDate = 0x40;
inline constexpr std::size_t ApplicationWriteCounter = 0x44;
inline constexpr std::size_t Version = 0x46;
inline constexpr std::size_t MiiV3 = 0x80;
inline constexpr std::size_t MiiStoreDataExtension = 0xE0;
inline constexpr std::size_t FirstWriteDate = 0xE8;
inline constexpr std::size_t AmiiboName = 0xEC;
inline constexpr std::size_t FontRegion = 0x102;
inline constexpr std::size_t Unknown1 = 0x103;
inline constexpr std::size_t RegisterInfoCrc = 0x104;
inline constexpr std::size_t Unknown2 = 0x108;
inline constexpr std::size_t ApplicationId = 0x180;
inline constexpr std::size_t AccessId = 0x188;
inline constexpr std::size_t CrcCounter = 0x18C;
inline constexpr std::size_t Flags = 0x18E;
inline constexpr std::size_t ApplicationIdByte = 0x191;
inline constexpr std::size_t ApplicationArea = 0x1C0;
inline constexpr std::size_t ApplicationAreaSize = 0xD8;
} // namespace NfpOffset

bool IsAmiiboReaderAvailable();

bool StartAmiiboScan(std::string& error);
void StopAmiiboScan();
bool IsAmiiboScanning();

bool PollAmiiboScan(AmiiboCapture& out, std::string& error);

bool IsAmiiboOnReader();

bool IsAmiiboSessionOpen();

bool RecaptureAmiibo(AmiiboCapture& out, std::string& error);

void ResumeAmiiboScanning();

enum class AmiiboWriteResult {
    Ok,
    NeedsOverwrite,
    Failed,
};

AmiiboWriteResult WriteAmiiboApplicationArea(std::uint32_t access_id, const std::uint8_t* data,
                                             std::size_t size, std::string& error);

bool OverwriteAmiiboApplicationArea(std::uint32_t access_id, const std::uint8_t* data,
                                    std::size_t size, std::string& error);

std::uint64_t GetAmiiboAreaOwner();

void ReleaseAmiibo();

} // namespace SwitchFrontend
