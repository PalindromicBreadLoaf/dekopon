// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstring>

#include "citra_switch/amiibo_convert.h"
#include "core/hle/service/nfc/nfc_types.h"

namespace SwitchFrontend {

namespace {

using namespace Service::NFC;

template <typename T>
T Read(const AmiiboCapture& capture, std::size_t offset) {
    T value{};
    std::memcpy(&value, capture.nfp_data.data() + offset, sizeof(T));
    return value;
}

AmiiboDate PackDate(const AmiiboCapture& capture, std::size_t offset) {
    AmiiboDate date{};
    date.SetYear(Read<u16>(capture, offset));
    date.SetMonth(Read<u8>(capture, offset + 2));
    date.SetDay(Read<u8>(capture, offset + 3));
    return date;
}

void FillUid(NTAG215File& tag, const std::array<u8, 7>& uid) {
    constexpr u8 cascade_tag = 0x88; // ISO/IEC 14443-3
    tag.uid[0] = uid[0];
    tag.uid[1] = uid[1];
    tag.uid[2] = uid[2];
    tag.uid[3] = static_cast<u8>(cascade_tag ^ uid[0] ^ uid[1] ^ uid[2]);
    tag.uid[4] = uid[3];
    tag.uid[5] = uid[4];
    tag.uid[6] = uid[5];
    tag.nintendo_id = uid[6];
    tag.lock_bytes[0] = static_cast<u8>(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]);
    tag.lock_bytes[1] = 0x48; // NTAG215 internal byte
}

NTAG215Password BuildPassword(const std::array<u8, 7>& uid) {
    const std::array<u8, 4> pwd{
        static_cast<u8>(0xAA ^ uid[1] ^ uid[3]),
        static_cast<u8>(0x55 ^ uid[2] ^ uid[4]),
        static_cast<u8>(0xAA ^ uid[3] ^ uid[5]),
        static_cast<u8>(0x55 ^ uid[4] ^ uid[6]),
    };

    NTAG215Password password{};
    std::memcpy(&password.PWD, pwd.data(), pwd.size());
    password.PACK = 0x8080;
    password.RFUI = 0;
    return password;
}

} // Anonymous namespace

std::array<std::uint8_t, 0x21C> BuildPlainTag(const AmiiboCapture& capture) {
    NTAG215File tag{};

    FillUid(tag, capture.uid);
    tag.static_lock = 0xE00F;
    tag.compability_container = 0xEEFF10F1U;
    tag.dynamic_lock = 0xBD0F0001U;
    tag.CFG0 = 0x04000000U;
    tag.CFG1 = 0x5F;
    tag.password = BuildPassword(capture.uid);

    tag.constant_value = Read<u8>(capture, NfpOffset::TagMagic);
    tag.write_counter = Read<u16>(capture, NfpOffset::TagWriteCounter);
    tag.amiibo_version = static_cast<u8>(Read<u16>(capture, NfpOffset::Version));

    const u8 flags = Read<u8>(capture, NfpOffset::Flags);
    auto& settings = tag.settings;
    settings.settings.raw = static_cast<u8>((flags & 0x3) << 4);
    settings.country_code_id = Read<u8>(capture, NfpOffset::FontRegion);
    settings.crc_counter = Read<u16>(capture, NfpOffset::CrcCounter);
    settings.crc = Read<u32>(capture, NfpOffset::SettingsCrc);
    settings.init_date = PackDate(capture, NfpOffset::FirstWriteDate);
    settings.write_date = PackDate(capture, NfpOffset::LastWriteDate);
    for (std::size_t i = 0; i < settings.amiibo_name.size(); ++i) {
        settings.amiibo_name[i] =
            Read<u16>(capture, NfpOffset::AmiiboName + i * sizeof(u16));
    }

    std::memcpy(&tag.owner_mii, capture.nfp_data.data() + NfpOffset::MiiV3,
                sizeof(tag.owner_mii));
    tag.mii_extension = Read<u64>(capture, NfpOffset::MiiStoreDataExtension);
    tag.register_info_crc = Read<u32>(capture, NfpOffset::RegisterInfoCrc);
    for (std::size_t i = 0; i < tag.unknown2.size(); ++i) {
        tag.unknown2[i] = Read<u32>(capture, NfpOffset::Unknown2 + i * sizeof(u32));
    }
    tag.unknown = Read<u8>(capture, NfpOffset::Unknown1);

    tag.application_id = Read<u64>(capture, NfpOffset::ApplicationId);
    tag.application_id_byte = Read<u8>(capture, NfpOffset::ApplicationIdByte);
    tag.application_area_id = Read<u32>(capture, NfpOffset::AccessId);
    tag.application_write_counter = Read<u16>(capture, NfpOffset::ApplicationWriteCounter);
    std::memcpy(tag.application_area.data(), capture.nfp_data.data() + NfpOffset::ApplicationArea,
                tag.application_area.size());

    tag.model_info.character_id = capture.character_id;
    tag.model_info.character_variant = capture.character_variant;
    tag.model_info.amiibo_type = static_cast<AmiiboType>(capture.amiibo_type);
    tag.model_info.model_number = capture.model_number;
    tag.model_info.series = static_cast<AmiiboSeries>(capture.series);
    tag.model_info.tag_type = PackedTagType::Type2;

    std::array<std::uint8_t, 0x21C> out{};
    static_assert(sizeof(NTAG215File) == out.size());
    std::memcpy(out.data(), &tag, out.size());
    return out;
}

} // namespace SwitchFrontend
