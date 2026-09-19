// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>

#include "common/settings.h"
#include "video_core/frame_generation.h"

namespace VideoCore {

namespace {

constexpr u32 kWindowVBlanks = 60;

constexpr double kGuestRefreshHz = 60.0;

constexpr double kEngageMargin = 1.4;
constexpr double kDisengageMargin = 1.6;

constexpr double kPresentCapHz = 60.0;
constexpr double kHighRefreshPresentCapHz = 120.0;

std::atomic<u32> guest_frames{};
std::atomic<u32> guest_vblanks{};
std::atomic<FrameGenerationState> published_state{FrameGenerationState::Off};
std::atomic<u32> published_multiplier{};

u32 window_frames{};
u32 window_vblanks{};
bool classified{};
u32 active_multiplier{};

u32 FitMultiplier(double ratio, double cap, u32 ceiling, u32 current) {
    for (u32 multiplier = ceiling; multiplier >= 2; --multiplier) {
        const double budget = cap / (kGuestRefreshHz * multiplier);
        const double margin = multiplier == current ? kDisengageMargin : kEngageMargin;
        if (ratio < budget * margin) {
            return multiplier;
        }
    }
    return 0;
}

} // Anonymous namespace

const char* FrameGenerationStateName(FrameGenerationState state) {
    switch (state) {
    case FrameGenerationState::Waiting:
        return "Waiting";
    case FrameGenerationState::Bypassed:
        return "Bypassed";
    case FrameGenerationState::Active:
        return "Active";
    case FrameGenerationState::Unavailable:
        return "Unavailable";
    case FrameGenerationState::Off:
        break;
    }
    return "Off";
}

void CountGuestFrame() {
    guest_frames.fetch_add(1, std::memory_order_relaxed);
}

void CountGuestVBlank() {
    guest_vblanks.fetch_add(1, std::memory_order_relaxed);
}

FrameGenerationDecision UpdateFrameGenerationGate() {
    const u32 frames = guest_frames.load(std::memory_order_relaxed);
    const u32 vblanks = guest_vblanks.load(std::memory_order_relaxed);

    if (!Settings::values.use_frame_generation.GetValue()) {
        window_frames = frames;
        window_vblanks = vblanks;
        classified = false;
        active_multiplier = 0;
        return {};
    }

    const u32 elapsed = vblanks - window_vblanks;
    if (elapsed >= kWindowVBlanks) {
        const double cap = Settings::values.frame_generation_high_refresh.GetValue()
                               ? kHighRefreshPresentCapHz
                               : kPresentCapHz;
        const double ratio = static_cast<double>(frames - window_frames) / elapsed;
        active_multiplier =
            FitMultiplier(ratio, cap, Settings::values.frame_generation_multiplier.GetValue(),
                          active_multiplier);
        classified = true;
        window_frames = frames;
        window_vblanks = vblanks;
    }

    if (!classified) {
        return {FrameGenerationState::Waiting, 0};
    }
    if (active_multiplier == 0) {
        return {FrameGenerationState::Bypassed, 0};
    }
    return {FrameGenerationState::Active, active_multiplier};
}

void PublishFrameGenerationDecision(const FrameGenerationDecision& decision) {
    published_state.store(decision.state, std::memory_order_relaxed);
    published_multiplier.store(decision.multiplier, std::memory_order_relaxed);
}

FrameGenerationDecision GetFrameGenerationDecision() {
    return {published_state.load(std::memory_order_relaxed),
            published_multiplier.load(std::memory_order_relaxed)};
}

} // namespace VideoCore
