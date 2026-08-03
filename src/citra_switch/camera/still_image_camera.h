// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include "core/frontend/camera/factory.h"
#include "core/frontend/camera/interface.h"

namespace Camera::StillImage {

// Decodes `path` into the cache every camera created afterwards draws from, so the picture is
// already in memory by the time the guest asks for a frame.
std::string Preload(const std::string& path);

// The camera the "image" factory hands out.
class Factory final : public CameraFactory {
public:
    std::unique_ptr<CameraInterface> Create(const std::string& config,
                                            const Service::CAM::Flip& flip) override;
};

} // namespace Camera::StillImage
