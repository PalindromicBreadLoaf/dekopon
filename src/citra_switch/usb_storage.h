// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <vector>

// USB mass storage.
namespace SwitchFrontend {

struct UsbVolume {
    std::string root;  // Devoptab path with a trailing '/', e.g. "ums0:/".
    std::string label; // Product name
};

bool InitUsbStorage();

void ShutdownUsbStorage();

// True once the host interface is up.
bool IsUsbStorageAvailable();

// Why InitUsbStorage() failed
const std::string& UsbStorageError();

// The volumes mounted right now.
std::vector<UsbVolume> GetUsbVolumes();

// Whether a drive has been attached or removed since the last call.
bool ConsumeUsbStorageChange();

} // namespace SwitchFrontend
