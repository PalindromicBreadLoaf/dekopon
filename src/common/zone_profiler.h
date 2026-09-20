// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <string>
#include "common/arch.h"
#include "common/common_types.h"

namespace Common::Profiling {

#ifdef DEKOPON_PROFILING
inline constexpr bool Enabled = true;
#else
inline constexpr bool Enabled = false;
#endif

inline constexpr u32 MaxZones = 128;
inline constexpr u32 MaxCounters = 32;
inline constexpr u32 MaxLabelLength = 32;

#ifdef DEKOPON_PROFILING

namespace detail {

[[nodiscard]] inline u64 ReadTick() {
#if CITRA_ARCH(arm64)
    u64 ticks;
    asm volatile("mrs %0, cntpct_el0" : "=r"(ticks));
    return ticks;
#else
    return static_cast<u64>(
        std::chrono::steady_clock::now().time_since_epoch().count());
#endif
}

class Scope;

struct ThreadState {
    std::atomic<u64> self_ticks[MaxZones]{};
    std::atomic<u32> calls[MaxZones]{};
    u64 reported_ticks[MaxZones]{};
    u32 reported_calls[MaxZones]{};
    std::atomic<u64> counter_sum[MaxCounters]{};
    std::atomic<u64> counter_max[MaxCounters]{};
    std::atomic<u32> counter_hits[MaxCounters]{};
    u64 reported_counter_sum[MaxCounters]{};
    u32 reported_counter_hits[MaxCounters]{};
    Scope* current{};
    char label[MaxLabelLength]{};
    ThreadState* next{};
};

[[nodiscard]] ThreadState& GetThreadState();

} // namespace detail

[[nodiscard]] u32 RegisterZone(const char* group, const char* name);

[[nodiscard]] u32 RegisterCounter(const char* group, const char* name);

struct CounterHandle {
    CounterHandle(const char* group, const char* name) : index{RegisterCounter(group, name)} {}
    u32 index;
};

inline void AddCounter(const CounterHandle& counter, u64 value) {
    auto& state = detail::GetThreadState();
    auto& sum = state.counter_sum[counter.index];
    sum.store(sum.load(std::memory_order_relaxed) + value, std::memory_order_relaxed);
    auto& peak = state.counter_max[counter.index];
    if (value > peak.load(std::memory_order_relaxed)) {
        peak.store(value, std::memory_order_relaxed);
    }
    auto& hits = state.counter_hits[counter.index];
    hits.store(hits.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}

struct ZoneHandle {
    ZoneHandle(const char* group, const char* name) : index{RegisterZone(group, name)} {}
    u32 index;
};

namespace detail {

class Scope {
public:
    explicit Scope(const ZoneHandle& zone)
        : state{&GetThreadState()}, parent{state->current}, index{zone.index} {
        state->current = this;
        start = ReadTick();
    }

    ~Scope() {
        const u64 elapsed = ReadTick() - start;
        auto& self = state->self_ticks[index];
        self.store(self.load(std::memory_order_relaxed) + (elapsed - child_ticks),
                   std::memory_order_relaxed);
        auto& count = state->calls[index];
        count.store(count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

        state->current = parent;
        if (parent) {
            parent->child_ticks += elapsed;
        }
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    ThreadState* state;
    Scope* parent;
    u64 start{};
    u64 child_ticks{};
    u32 index;
};

} // namespace detail

void SetThreadLabel(const char* label);

void MarkFrame();

[[nodiscard]] std::string Consume();

#else

struct ZoneHandle {
    constexpr ZoneHandle(const char*, const char*) {}
};

struct CounterHandle {
    constexpr CounterHandle(const char*, const char*) {}
};

inline void AddCounter(const CounterHandle&, u64) {}

namespace detail {
class Scope {
public:
    explicit Scope(const ZoneHandle&) {}
};
} // namespace detail

inline void SetThreadLabel(const char*) {}
inline void MarkFrame() {}
[[nodiscard]] inline std::string Consume() {
    return {};
}

#endif // DEKOPON_PROFILING

} // namespace Common::Profiling

#define CITRA_PROFILE_CAT_IMPL(a, b) a##b
#define CITRA_PROFILE_CAT(a, b) CITRA_PROFILE_CAT_IMPL(a, b)
#define CITRA_PROFILE_ZONE_DEFINE(ident, group, name)                                              \
    ::Common::Profiling::ZoneHandle g_zone_##ident { group, name }
#define CITRA_PROFILE_ZONE_DECLARE(ident) extern ::Common::Profiling::ZoneHandle g_zone_##ident
#define CITRA_PROFILE_SCOPE(ident)                                                                 \
    const ::Common::Profiling::detail::Scope CITRA_PROFILE_CAT(prof_scope_, __LINE__) {            \
        g_zone_##ident                                                                             \
    }

#define CITRA_PROFILE_COUNTER_DEFINE(ident, group, name)                                           \
    ::Common::Profiling::CounterHandle g_counter_##ident { group, name }
#define CITRA_PROFILE_COUNT(ident, value) ::Common::Profiling::AddCounter(g_counter_##ident, value)
