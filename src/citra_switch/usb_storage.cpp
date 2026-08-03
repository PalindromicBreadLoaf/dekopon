// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdio>
#include <mutex>
#include <vector>
#include <switch.h>
#include <usbhsfs.h>

#include "citra_switch/usb_storage.h"

namespace SwitchFrontend {

namespace {

std::mutex g_volumes_mutex;
std::vector<UsbVolume> g_volumes;
std::atomic<bool> g_changed{false};
bool g_initialized = false;
std::string g_error;

UsbVolume ToVolume(const UsbHsFsDevice& device) {
    UsbVolume volume;
    // `name` is the devoptab prefix ("ums0:") so a '/' turns it into the volume's root.
    volume.root = std::string{device.name} + '/';
    volume.label = device.product_name[0] != '\0' ? device.product_name : device.name;
    return volume;
}

void StoreVolumes(std::vector<UsbVolume> volumes) {
    {
        const std::lock_guard lock{g_volumes_mutex};
        g_volumes = std::move(volumes);
    }
    g_changed.store(true, std::memory_order_release);
}

// Runs on libusbhsfs' own thread whenever a drive is attached or removed.
void PopulateCallback(const UsbHsFsDevice* devices, u32 device_count, void*) {
    std::vector<UsbVolume> volumes;
    volumes.reserve(device_count);
    for (u32 i = 0; devices != nullptr && i < device_count; ++i) {
        volumes.push_back(ToVolume(devices[i]));
    }
    StoreVolumes(std::move(volumes));
}

void RefreshVolumes() {
    const u32 count = usbHsFsGetMountedDeviceCount();
    if (count == 0) {
        StoreVolumes({});
        return;
    }

    std::vector<UsbHsFsDevice> devices(count);
    const u32 listed = usbHsFsListMountedDevices(devices.data(), count);
    std::vector<UsbVolume> volumes;
    volumes.reserve(listed);
    for (u32 i = 0; i < listed; ++i) {
        volumes.push_back(ToVolume(devices[i]));
    }
    StoreVolumes(std::move(volumes));
}

} // namespace

bool InitUsbStorage() {
    if (g_initialized) {
        return true;
    }

    const Result rc = usbHsFsInitialize(0);
    if (R_FAILED(rc)) {
        char message[96];
        std::snprintf(message, sizeof(message), "USB storage unavailable (0x%08X)",
                      static_cast<unsigned>(rc));
        g_error = message;
        return false;
    }

    g_initialized = true;
    g_error.clear();
    usbHsFsSetPopulateCallback(PopulateCallback, nullptr);
    RefreshVolumes();
    return true;
}

void ShutdownUsbStorage() {
    if (!g_initialized) {
        return;
    }
    usbHsFsSetPopulateCallback(nullptr, nullptr);
    usbHsFsExit();
    g_initialized = false;
    StoreVolumes({});
    g_changed.store(false, std::memory_order_release);
}

bool IsUsbStorageAvailable() {
    return g_initialized;
}

const std::string& UsbStorageError() {
    return g_error;
}

std::vector<UsbVolume> GetUsbVolumes() {
    const std::lock_guard lock{g_volumes_mutex};
    return g_volumes;
}

bool ConsumeUsbStorageChange() {
    return g_changed.exchange(false, std::memory_order_acq_rel);
}

} // namespace SwitchFrontend
