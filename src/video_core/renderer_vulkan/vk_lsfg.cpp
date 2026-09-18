// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_lsfg.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>

#include "common/file_util.h"
#include "common/logging/log.h"

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

namespace Vulkan {

namespace {

constexpr float kMinFlowScale = 0.125f;
constexpr float kMaxFlowScale = 1.0f;

std::string GetPipelineCachePath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + "lsfg-pipeline-cache.bin";
}

} // Anonymous namespace

class LsfgBridge {
public:
    LsfgBridge(const LsfgBridgeInfo& info, const std::string& dll_path,
               const std::string& cache_path)
        : width{info.width}, height{info.height} {
        const lsfgvk::backend::BorrowedDevice borrowed{
            .instance = info.instance,
            .physicalDevice = info.physical_device,
            .device = info.device,
            .queueFamilyIndex = info.queue_family_index,
            .queue = info.queue,
            .getInstanceProcAddr = LoadInstanceProcAddr(),
            .pipelineCachePath = std::filesystem::path{cache_path},
        };

        float flow_scale = info.flow_scale;
        if (!std::isfinite(flow_scale)) {
            flow_scale = 0.25f;
        }
        flow_scale = std::clamp(flow_scale, kMinFlowScale, kMaxFlowScale);

        backend = std::make_unique<lsfgvk::backend::Instance>(
            borrowed, std::filesystem::path{dll_path}, false);
        context = &backend->openLocalContext(width, height, false, 1.0f / flow_scale,
                                             info.performance_mode, 1, VK_QUEUE_FAMILY_IGNORED);

        LOG_INFO(Render_Vulkan,
                 "LSFG context opened at {}x{} (flow scale {}, {} mode, cache {})", width, height,
                 flow_scale, info.performance_mode ? "performance" : "quality", cache_path);
    }

    ~LsfgBridge() {
        const auto& funcs = backend->vulkan().df();
        if (funcs.DeviceWaitIdle) {
            funcs.DeviceWaitIdle(backend->vulkan().dev());
        }
    }

    LsfgBridge(const LsfgBridge&) = delete;
    LsfgBridge& operator=(const LsfgBridge&) = delete;

private:
    static PFN_vkGetInstanceProcAddr LoadInstanceProcAddr() {
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(&vk_icdGetInstanceProcAddr);
    }

    std::unique_ptr<lsfgvk::backend::Instance> backend;
    lsfgvk::backend::Context* context{};
    u32 width;
    u32 height;
};

void LsfgBridgeDeleter::operator()(LsfgBridge* bridge) const noexcept {
    delete bridge;
}

std::string GetLsfgShaderDllPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "lsfg/Lossless.dll";
}

bool IsLsfgShaderDllPresent() {
    return FileUtil::Exists(GetLsfgShaderDllPath());
}

LsfgBridgePtr CreateLsfgBridge(const LsfgBridgeInfo& info) {
    const std::string dll_path = GetLsfgShaderDllPath();
    if (!FileUtil::Exists(dll_path)) {
        LOG_WARNING(Render_Vulkan, "Frame generation is on but {} is missing",
                    dll_path);
        return {};
    }

    const std::string cache_path = GetPipelineCachePath();
    FileUtil::CreateFullPath(cache_path);

    try {
        return LsfgBridgePtr{new LsfgBridge{info, dll_path, cache_path}};
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Failed to open LSFG context: {}", e.what());
        return {};
    }
}

} // namespace Vulkan
