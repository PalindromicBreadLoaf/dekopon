// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "common/thread_worker.h"

namespace Common {

namespace {

struct Registration {
    void* worker;
    detail::ThreadWorkerStopFn stop;
};

std::mutex g_registry_mutex;
std::vector<Registration> g_registry;
std::once_flag g_atexit_once;

} // Anonymous namespace

namespace detail {

void RegisterThreadWorker(void* worker, ThreadWorkerStopFn stop) {
    std::call_once(g_atexit_once, [] { std::atexit(&StopAllThreadWorkers); });

    std::lock_guard lock{g_registry_mutex};
    g_registry.push_back({worker, stop});
}

void UnregisterThreadWorker(void* worker) {
    std::lock_guard lock{g_registry_mutex};
    const auto it = std::find_if(g_registry.begin(), g_registry.end(),
                                 [worker](const Registration& entry) {
                                     return entry.worker == worker;
                                 });
    if (it != g_registry.end()) {
        g_registry.erase(it);
    }
}

} // namespace detail

void StopAllThreadWorkers() {
    for (;;) {
        Registration entry;
        {
            std::lock_guard lock{g_registry_mutex};
            if (g_registry.empty()) {
                return;
            }
            entry = g_registry.back();
            g_registry.pop_back();
        }
        entry.stop(entry.worker);
    }
}

} // namespace Common
