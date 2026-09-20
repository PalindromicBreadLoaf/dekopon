// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/zone_profiler.h"

#ifdef DEKOPON_PROFILING

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>
#include <fmt/format.h>

namespace Common::Profiling {

namespace {

struct ZoneInfo {
    const char* group;
    const char* name;
};

struct Registry {
    std::mutex mutex;
    ZoneInfo zones[MaxZones]{};
    u32 count{};
    ZoneInfo counters[MaxCounters]{};
    u32 counter_count{};
};

Registry& GetRegistry() {
    static Registry registry;
    return registry;
}

std::atomic<detail::ThreadState*> g_threads{nullptr};
std::atomic<u64> g_frames{0};
std::atomic<u64> g_last_consume_tick{0};

u64 TickFrequency() {
    static const u64 frequency = [] {
#if CITRA_ARCH(arm64)
        u64 value;
        asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
        return value ? value : 19'200'000;
#else
        return static_cast<u64>(std::chrono::steady_clock::period::den);
#endif
    }();
    return frequency;
}

double TicksToMs(u64 ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(TickFrequency());
}

struct Row {
    const char* group;
    const char* name;
    u64 ticks;
    u32 calls;
};

} // Anonymous namespace

u32 RegisterZone(const char* group, const char* name) {
    Registry& registry = GetRegistry();
    std::lock_guard lock{registry.mutex};

    for (u32 i = 0; i < registry.count; i++) {
        if (registry.zones[i].group == group && registry.zones[i].name == name) {
            return i;
        }
    }
    if (registry.count == MaxZones) {
        return MaxZones - 1;
    }
    registry.zones[registry.count] = {group, name};
    return registry.count++;
}

u32 RegisterCounter(const char* group, const char* name) {
    Registry& registry = GetRegistry();
    std::lock_guard lock{registry.mutex};

    for (u32 i = 0; i < registry.counter_count; i++) {
        if (registry.counters[i].group == group && registry.counters[i].name == name) {
            return i;
        }
    }
    if (registry.counter_count == MaxCounters) {
        return MaxCounters - 1;
    }
    registry.counters[registry.counter_count] = {group, name};
    return registry.counter_count++;
}

namespace detail {

ThreadState& GetThreadState() {
    static thread_local ThreadState* state = [] {
        auto* fresh = new ThreadState{};
        std::strncpy(fresh->label, "unnamed", MaxLabelLength - 1);
        fresh->next = g_threads.load(std::memory_order_relaxed);
        while (!g_threads.compare_exchange_weak(fresh->next, fresh, std::memory_order_release,
                                                std::memory_order_relaxed)) {
        }
        return fresh;
    }();
    return *state;
}

} // namespace detail

void SetThreadLabel(const char* label) {
    if (!label) {
        return;
    }
    auto& state = detail::GetThreadState();
    std::strncpy(state.label, label, MaxLabelLength - 1);
    state.label[MaxLabelLength - 1] = '\0';
}

void MarkFrame() {
    g_frames.fetch_add(1, std::memory_order_relaxed);
}

std::string Consume() {
    const u64 now = detail::ReadTick();
    const u64 previous = g_last_consume_tick.exchange(now, std::memory_order_relaxed);
    const u64 frames = g_frames.exchange(0, std::memory_order_relaxed);

    if (previous == 0) {
        for (auto* state = g_threads.load(std::memory_order_acquire); state; state = state->next) {
            for (u32 i = 0; i < MaxZones; i++) {
                state->reported_ticks[i] = state->self_ticks[i].load(std::memory_order_relaxed);
                state->reported_calls[i] = state->calls[i].load(std::memory_order_relaxed);
            }
            for (u32 i = 0; i < MaxCounters; i++) {
                state->reported_counter_sum[i] =
                    state->counter_sum[i].load(std::memory_order_relaxed);
                state->reported_counter_hits[i] =
                    state->counter_hits[i].load(std::memory_order_relaxed);
                state->counter_max[i].store(0, std::memory_order_relaxed);
            }
        }
        return {};
    }

    const u64 interval_ticks = now - previous;
    if (interval_ticks == 0) {
        return {};
    }

    u32 zone_count;
    u32 counter_count;
    ZoneInfo zones[MaxZones];
    ZoneInfo counters[MaxCounters];
    {
        Registry& registry = GetRegistry();
        std::lock_guard lock{registry.mutex};
        zone_count = registry.count;
        std::copy_n(registry.zones, zone_count, zones);
        counter_count = registry.counter_count;
        std::copy_n(registry.counters, counter_count, counters);
    }

    const double interval_ms = TicksToMs(interval_ticks);
    std::string report;

    for (auto* state = g_threads.load(std::memory_order_acquire); state; state = state->next) {
        std::vector<Row> rows;
        u64 total_ticks = 0;
        for (u32 i = 0; i < zone_count; i++) {
            const u64 total_zone_ticks = state->self_ticks[i].load(std::memory_order_relaxed);
            const u32 total_zone_calls = state->calls[i].load(std::memory_order_relaxed);
            const u64 ticks = total_zone_ticks - state->reported_ticks[i];
            const u32 calls = total_zone_calls - state->reported_calls[i];
            state->reported_ticks[i] = total_zone_ticks;
            state->reported_calls[i] = total_zone_calls;
            if (ticks == 0 && calls == 0) {
                continue;
            }
            total_ticks += ticks;
            rows.push_back({zones[i].group, zones[i].name, ticks, calls});
        }
        std::string counter_report;
        for (u32 i = 0; i < counter_count; i++) {
            const u64 sum = state->counter_sum[i].load(std::memory_order_relaxed);
            const u32 hits = state->counter_hits[i].load(std::memory_order_relaxed);
            const u64 peak = state->counter_max[i].exchange(0, std::memory_order_relaxed);
            const u64 delta_sum = sum - state->reported_counter_sum[i];
            const u32 delta_hits = hits - state->reported_counter_hits[i];
            state->reported_counter_sum[i] = sum;
            state->reported_counter_hits[i] = hits;
            if (delta_hits == 0) {
                continue;
            }
            counter_report +=
                fmt::format("\n      mean {:>12} max {:>12} {:>9.1f} hits/f  {}/{}",
                            delta_sum / delta_hits, peak,
                            frames ? static_cast<double>(delta_hits) / static_cast<double>(frames)
                                   : 0.0,
                            counters[i].group, counters[i].name);
        }

        if (rows.empty() && counter_report.empty()) {
            continue;
        }

        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.ticks > b.ticks; });

        report += fmt::format("\n  [{}] {:.1f}% of wall in profiled zones", state->label,
                              100.0 * TicksToMs(total_ticks) / interval_ms);
        for (const Row& row : rows) {
            const double zone_ms = TicksToMs(row.ticks);
            if (zone_ms / interval_ms < 0.001 && row.calls < 100) {
                continue;
            }
            const double per_frame = frames ? 1.0 / static_cast<double>(frames) : 0.0;
            report += fmt::format("\n      {:>6.2f}% {:>8.3f} ms/f {:>9.1f} calls/f  {}/{}",
                                  100.0 * zone_ms / interval_ms, zone_ms * per_frame,
                                  static_cast<double>(row.calls) * per_frame, row.group, row.name);
        }
        report += counter_report;
    }

    if (report.empty()) {
        return {};
    }
    return fmt::format("Zone profile over {:.1f} s, {} frames:{}", interval_ms / 1000.0, frames,
                       report);
}

} // namespace Common::Profiling

#endif // DEKOPON_PROFILING
