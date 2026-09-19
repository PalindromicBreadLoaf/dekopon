// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <vulkan/vulkan_core.h>
#include "common/common_types.h"

namespace Vulkan {

struct LsfgBridgeInfo {
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    u32 queue_family_index;
    u32 width;
    u32 height;
    float flow_scale;
    bool performance_mode;
};

class LsfgBridge {
public:
    virtual ~LsfgBridge() = default;

    virtual VkImage RecordFrame(VkImage frame_image, VkSemaphore render_ready) = 0;
};

using LsfgBridgePtr = std::unique_ptr<LsfgBridge>;

std::string GetLsfgShaderDllPath();

bool IsLsfgShaderDllPresent();

LsfgBridgePtr CreateLsfgBridge(const LsfgBridgeInfo& info);

} // namespace Vulkan
