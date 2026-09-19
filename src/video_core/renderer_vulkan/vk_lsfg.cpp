// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vk_lsfg.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <filesystem>
#include <vector>

#include "common/file_util.h"
#include "common/logging/log.h"

#include "lsfg-vk-backend/lsfgvk.hpp"
#include "lsfg-vk-common/vulkan/command_buffer.hpp"
#include "lsfg-vk-common/vulkan/fence.hpp"
#include "lsfg-vk-common/vulkan/vulkan.hpp"

extern "C" PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName);

namespace Vulkan {

namespace {

constexpr float kMinFlowScale = 0.125f;
constexpr float kMaxFlowScale = 1.0f;

constexpr size_t kSlotCount = 3;

std::string GetPipelineCachePath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::CacheDir) + "lsfg-pipeline-cache.bin";
}

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access, VkAccessFlags dst_access,
                                 VkImageLayout old_layout, VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    };
}

} // Anonymous namespace

class LsfgBridgeImpl final : public LsfgBridge {
public:
    LsfgBridgeImpl(const LsfgBridgeInfo& info, const std::string& dll_path,
                   const std::string& cache_path)
        : extent{info.width, info.height} {
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
        context = &backend->openLocalContext(extent.width, extent.height, false, 1.0f / flow_scale,
                                             info.performance_mode, 1, VK_QUEUE_FAMILY_IGNORED);
        vulkan = &backend->vulkan();

        slots.reserve(kSlotCount);
        for (size_t i = 0; i < kSlotCount; ++i) {
            slots.emplace_back(*vulkan);
        }

        LOG_INFO(Render_Vulkan, "LSFG context opened at {}x{} (flow scale {}, {} mode, cache {})",
                 extent.width, extent.height, flow_scale,
                 info.performance_mode ? "performance" : "quality", cache_path);
    }

    ~LsfgBridgeImpl() override {
        const auto& funcs = backend->vulkan().df();
        if (funcs.DeviceWaitIdle) {
            funcs.DeviceWaitIdle(backend->vulkan().dev());
        }
    }

    LsfgBridgeImpl(const LsfgBridgeImpl&) = delete;
    LsfgBridgeImpl& operator=(const LsfgBridgeImpl&) = delete;

    VkImage RecordFrame(VkImage frame_image, VkSemaphore render_ready) override {
        Slot& slot = AcquireSlot();

        const size_t source_index = static_cast<size_t>((frame_index + 1) & 1);
        const VkImage source = backend->sourceImage(*context, source_index);
        const bool initialized = source_initialized[source_index];
        const bool interpolate = frame_index != 0;

        slot.cmdbuf.begin(*vulkan);
        slot.cmdbuf.copyImage(
            *vulkan,
            {
                MakeBarrier(frame_image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                MakeBarrier(source, initialized ? VK_ACCESS_SHADER_READ_BIT : 0,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
            },
            {frame_image, source}, extent,
            {
                MakeBarrier(frame_image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
                MakeBarrier(source, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
            });

        if (interpolate) {
            backend->recordFrame(*context, slot.cmdbuf);
        }

        slot.cmdbuf.end(*vulkan);
        slot.cmdbuf.submit(*vulkan, {render_ready}, VK_NULL_HANDLE, 0, {}, VK_NULL_HANDLE, 0,
                           slot.done.handle());
        slot.pending = true;

        source_initialized[source_index] = true;
        ++frame_index;

        return interpolate ? backend->destinationImage(*context, 0) : VK_NULL_HANDLE;
    }

private:
    struct Slot {
        explicit Slot(const vk::Vulkan& vulkan) : cmdbuf{vulkan}, done{vulkan} {}

        vk::CommandBuffer cmdbuf;
        vk::Fence done;
        bool pending{};
    };

    static PFN_vkGetInstanceProcAddr LoadInstanceProcAddr() {
        return reinterpret_cast<PFN_vkGetInstanceProcAddr>(&vk_icdGetInstanceProcAddr);
    }

    Slot& AcquireSlot() {
        Slot& slot = slots[slot_cursor++ % slots.size()];
        if (slot.pending) {
            if (!slot.done.wait(*vulkan)) {
                throw lsfgvk::backend::error("Timed out waiting for an LSFG capture slot");
            }
            slot.pending = false;
        }
        slot.done.reset(*vulkan);
        return slot;
    }

    std::unique_ptr<lsfgvk::backend::Instance> backend;
    lsfgvk::backend::Context* context{};
    const vk::Vulkan* vulkan{};
    std::vector<Slot> slots;
    std::array<bool, 2> source_initialized{};
    VkExtent2D extent;
    u64 frame_index{};
    size_t slot_cursor{};
};

std::string GetLsfgShaderDllPath() {
    return FileUtil::GetUserPath(FileUtil::UserPath::UserDir) + "lsfg/Lossless.dll";
}

bool IsLsfgShaderDllPresent() {
    return FileUtil::Exists(GetLsfgShaderDllPath());
}

LsfgBridgePtr CreateLsfgBridge(const LsfgBridgeInfo& info) {
    const std::string dll_path = GetLsfgShaderDllPath();
    if (!FileUtil::Exists(dll_path)) {
        LOG_WARNING(Render_Vulkan, "Frame generation is on but {} is missing", dll_path);
        return {};
    }

    const std::string cache_path = GetPipelineCachePath();
    FileUtil::CreateFullPath(cache_path);

    try {
        return std::make_unique<LsfgBridgeImpl>(info, dll_path, cache_path);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "Failed to open LSFG context: {}", e.what());
        return {};
    }
}

} // namespace Vulkan
