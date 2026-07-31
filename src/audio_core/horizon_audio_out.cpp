// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include "audio_core/horizon_audio_out.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <switch.h>
#include "common/horizon_thread.h"

// Libnx-based audio output made specifically for the Switch
namespace AudioCore::Horizon {

namespace {

constexpr std::size_t Channels = 2;
constexpr std::size_t BufferFrames = 1024;
constexpr std::size_t BufferBytes = BufferFrames * Channels * sizeof(std::int16_t);
constexpr std::size_t BufferCount = 4;
constexpr std::uint64_t WaitTimeoutNs = 100'000'000;

static_assert(BufferBytes % 0x1000 == 0);

} // namespace

struct AudioOut {
    std::array<AudioOutBuffer, BufferCount> buffers{};
    std::array<void*, BufferCount> memory{};
    std::thread thread;
    std::atomic<bool> quit{};
    FillCallback callback{};
    void* user_data{};
    std::uint32_t sample_rate{};
    std::uint32_t channel_count{};
    bool initialized{};
    bool started{};
};

namespace {

void FillBuffer(AudioOut& audio_out, AudioOutBuffer& buffer) {
    audio_out.callback(audio_out.user_data, static_cast<std::int16_t*>(buffer.buffer),
                       BufferFrames);
    buffer.data_offset = 0;
    buffer.data_size = BufferBytes;
}

void AudioThread(AudioOut* audio_out) {
    Common::Horizon::PinCurrentThread(Common::Horizon::CoreAudio);
    // Above the 44 default (lower is higher) so this thread isn't preempted right when it needs to append its next buffer.
    svcSetThreadPriority(CUR_THREAD_HANDLE, 38);

    while (!audio_out->quit.load(std::memory_order_relaxed)) {
        AudioOutBuffer* released = nullptr;
        std::uint32_t released_count = 0;
        const Result result = audoutWaitPlayFinish(&released, &released_count, WaitTimeoutNs);
        if (R_FAILED(result) || released_count == 0) {
            continue;
        }

        while (released != nullptr) {
            AudioOutBuffer* const next = released->next;
            FillBuffer(*audio_out, *released);
            audoutAppendAudioOutBuffer(released);
            released = next;
        }
    }
}

} // namespace

AudioOut* CreateAudioOut(std::uint32_t& result) {
    result = 0;
    auto* audio_out = new (std::nothrow) AudioOut;
    if (audio_out == nullptr) {
        result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        return nullptr;
    }

    result = audoutInitialize();
    if (R_FAILED(result)) {
        delete audio_out;
        return nullptr;
    }
    audio_out->initialized = true;
    audio_out->sample_rate = audoutGetSampleRate();
    audio_out->channel_count = audoutGetChannelCount();

    for (std::size_t i = 0; i < BufferCount; ++i) {
        void* const memory = std::aligned_alloc(0x1000, BufferBytes);
        if (memory == nullptr) {
            result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
            DestroyAudioOut(audio_out);
            return nullptr;
        }
        std::memset(memory, 0, BufferBytes);
        audio_out->memory[i] = memory;
        audio_out->buffers[i] = {
            .next = nullptr,
            .buffer = memory,
            .buffer_size = BufferBytes,
            .data_size = BufferBytes,
            .data_offset = 0,
        };
    }
    return audio_out;
}

std::uint32_t StartAudioOut(AudioOut* audio_out, FillCallback callback, void* user_data) {
    if (audio_out == nullptr || callback == nullptr || audio_out->started) {
        return MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }

    audio_out->callback = callback;
    audio_out->user_data = user_data;

    Result result = audoutStartAudioOut();
    if (R_FAILED(result)) {
        return result;
    }
    audio_out->started = true;

    for (AudioOutBuffer& buffer : audio_out->buffers) {
        FillBuffer(*audio_out, buffer);
        result = audoutAppendAudioOutBuffer(&buffer);
        if (R_FAILED(result)) {
            return result;
        }
    }

    audio_out->quit.store(false, std::memory_order_relaxed);
    audio_out->thread = std::thread(AudioThread, audio_out);
    return 0;
}

void DestroyAudioOut(AudioOut* audio_out) {
    if (audio_out == nullptr) {
        return;
    }

    audio_out->quit.store(true, std::memory_order_relaxed);
    if (audio_out->thread.joinable()) {
        audio_out->thread.join();
    }
    if (audio_out->started) {
        audoutStopAudioOut();
    }
    if (audio_out->initialized) {
        audoutExit();
    }
    for (void* memory : audio_out->memory) {
        std::free(memory);
    }
    delete audio_out;
}

std::uint32_t GetAudioOutSampleRate(const AudioOut* audio_out) {
    return audio_out != nullptr ? audio_out->sample_rate : 0;
}

std::uint32_t GetAudioOutChannelCount(const AudioOut* audio_out) {
    return audio_out != nullptr ? audio_out->channel_count : 0;
}

} // namespace AudioCore::Horizon
