// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCore {

enum class FrameGenerationState {
    Off,
    Waiting,     // Not enough guest frames yet to classify the title.
    Bypassed,    // The title draws too often to interpolate without slowing it down.
    Active,      // Interpolating.
    Unavailable, // The interpolation chain could not be opened.
};

struct FrameGenerationDecision {
    FrameGenerationState state{FrameGenerationState::Off};
    u32 multiplier{};
};

const char* FrameGenerationStateName(FrameGenerationState state);

void CountGuestFrame();
void CountGuestVBlank();

FrameGenerationDecision UpdateFrameGenerationGate();

void PublishFrameGenerationDecision(const FrameGenerationDecision& decision);
FrameGenerationDecision GetFrameGenerationDecision();

} // namespace VideoCore
