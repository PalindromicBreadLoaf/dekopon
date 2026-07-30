// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <mutex>

#include "video_core/overlay.h"

namespace VideoCore {

namespace {
std::mutex s_mutex;
OverlayMenuState s_state;
std::atomic<bool> s_visible{false};
std::atomic<u32> s_pending_shader_compiles{0};

std::atomic<u32> s_rotation{0};

std::mutex s_toast_mutex;
std::string s_toast;
std::chrono::steady_clock::time_point s_toast_until;
} // namespace

void SetOverlayRotation(u32 degrees) {
    s_rotation.store(degrees % 360 / 90 * 90, std::memory_order_release);
}

u32 GetOverlayRotation() {
    return s_rotation.load(std::memory_order_acquire);
}

void SetOverlayMenuState(const OverlayMenuState& state) {
    {
        std::scoped_lock lock{s_mutex};
        s_state = state;
    }
    s_visible.store(state.visible, std::memory_order_release);
}

OverlayMenuState GetOverlayMenuState() {
    std::scoped_lock lock{s_mutex};
    return s_state;
}

bool IsOverlayMenuVisible() {
    return s_visible.load(std::memory_order_acquire);
}

void NotifyShaderCompileBegin() {
    s_pending_shader_compiles.fetch_add(1, std::memory_order_acq_rel);
}

void NotifyShaderCompileEnd() {
    s_pending_shader_compiles.fetch_sub(1, std::memory_order_acq_rel);
}

u32 GetPendingShaderCompiles() {
    return s_pending_shader_compiles.load(std::memory_order_acquire);
}

void PostOverlayToast(const std::string& text, u32 duration_ms) {
    std::scoped_lock lock{s_toast_mutex};
    s_toast = text;
    s_toast_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
}

std::string GetOverlayToast() {
    std::scoped_lock lock{s_toast_mutex};
    if (s_toast.empty()) {
        return {};
    }
    if (std::chrono::steady_clock::now() >= s_toast_until) {
        s_toast.clear();
        return {};
    }
    return s_toast;
}

} // namespace VideoCore
