// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

namespace SwitchFrontend {

bool BeginRealAmiiboScan(std::string& message);
void CancelRealAmiiboScan();

bool IsRealAmiiboScanning();
bool IsRealAmiiboActive();

void UpdateRealAmiibo();

void EndRealAmiibo();

bool HasPendingAmiiboOverwrite();

std::string PendingAmiiboOverwriteOwner();

bool ConfirmAmiiboOverwrite(std::string& message);
void DiscardAmiiboOverwrite();

} // namespace SwitchFrontend
