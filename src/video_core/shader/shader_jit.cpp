// Copyright 2016-2026 Citra Emulator Project / Azahar Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include "common/arch.h"
#if CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)

#include <exception>
#include <new>
#include "common/assert.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/microprofile.h"
#include "video_core/shader/shader.h"
#include "video_core/shader/shader_interpreter.h"
#include "video_core/shader/shader_jit.h"
#if CITRA_ARCH(arm64)
#include "video_core/shader/shader_jit_a64_compiler.h"
#endif
#if CITRA_ARCH(x86_64)
#include "video_core/shader/shader_jit_x64_compiler.h"
#endif

namespace Pica::Shader {

#ifdef __SWITCH__

JitEngine::JitEngine()
    : compile_workers{1, "PICA Shader JIT", {}, {3, 0}},
      interpreter{std::make_unique<InterpreterEngine>()} {}
JitEngine::~JitEngine() = default;

void JitEngine::CompileEntry(CacheEntry* entry, std::shared_ptr<const ProgramCode> program_code,
                             std::shared_ptr<const SwizzleData> swizzle_data) {
    std::unique_ptr<JitShader> shader;
    if (!exec_memory_exhausted.load(std::memory_order_relaxed)) {
        try {
            shader = std::make_unique<JitShader>();
            shader->Compile(program_code.get(), swizzle_data.get());
        } catch (const std::bad_alloc&) {
            // The host has no executable memory left to back this shader.
            // Nothing is ever freed from the cache, so no later shader will fare any better.
            LOG_WARNING(HW_GPU, "Out of executable memory for the shader JIT. Falling back to the "
                                "interpreter for this and all subsequent shaders");
            exec_memory_exhausted.store(true, std::memory_order_relaxed);
            shader.reset();
        } catch (const std::exception& e) {
            LOG_ERROR(HW_GPU, "Failed to compile shader, falling back to the interpreter: {}",
                      e.what());
            shader.reset();
        }
    }
    entry->shader = std::move(shader);
    entry->ready.store(true, std::memory_order_release);
}

void JitEngine::SetupBatch(ShaderSetup& setup, u32 entry_point) {
    ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH);
    setup.entry_point = entry_point;

    setup.DoProgramCodeFixup();
    const u64 code_hash = setup.GetProgramCodeHash();
    const u64 swizzle_hash = setup.GetSwizzleDataHash();
    const u64 cache_key = Common::HashCombine(code_hash, swizzle_hash);

    CacheEntry* entry;
    bool is_new;
    {
        std::lock_guard lock{cache_mutex};
        auto [iter, inserted] = cache.try_emplace(cache_key);
        if (inserted) {
            iter->second = std::make_unique<CacheEntry>();
        }
        entry = iter->second.get();
        is_new = inserted;
    }

    if (is_new) {
        // Copy the bytecode off the live ShaderSetup so the compile job can run on another
        // thread without touching state the caller keeps mutating.
        auto program_code = std::make_shared<ProgramCode>(setup.GetProgramCode());
        auto swizzle_data = std::make_shared<SwizzleData>(setup.GetSwizzleData());
        compile_workers.QueueWork([this, entry, program_code, swizzle_data] {
            CompileEntry(entry, program_code, swizzle_data);
        });
    }

    // Anything still compiling falls back to the interpreter.
    setup.cached_shader =
        entry->ready.load(std::memory_order_acquire) ? entry->shader.get() : nullptr;
}

#else

JitEngine::JitEngine() : interpreter{std::make_unique<InterpreterEngine>()} {}
JitEngine::~JitEngine() = default;

void JitEngine::SetupBatch(ShaderSetup& setup, u32 entry_point) {
    ASSERT(entry_point < MAX_PROGRAM_CODE_LENGTH);
    setup.entry_point = entry_point;

    setup.DoProgramCodeFixup();
    const u64 code_hash = setup.GetProgramCodeHash();
    const u64 swizzle_hash = setup.GetSwizzleDataHash();

    const u64 cache_key = Common::HashCombine(code_hash, swizzle_hash);
    auto iter = cache.find(cache_key);
    if (iter != cache.end()) {
        setup.cached_shader = iter->second.get();
        return;
    }

    std::unique_ptr<JitShader> shader;
    if (!exec_memory_exhausted) {
        try {
            shader = std::make_unique<JitShader>();
            shader->Compile(&setup.GetProgramCode(), &setup.GetSwizzleData());
        } catch (const std::bad_alloc&) {
            // The host has no executable memory left to back this shader.
            // Nothing is ever freed from the cache, so no later shader will fare any better.
            LOG_WARNING(HW_GPU, "Out of executable memory for the shader JIT, falling back to the "
                                "interpreter for this and all subsequent shaders");
            exec_memory_exhausted = true;
            shader.reset();
        } catch (const std::exception& e) {
            LOG_ERROR(HW_GPU, "Failed to compile shader, falling back to the interpreter: {}",
                      e.what());
            shader.reset();
        }
    }

    setup.cached_shader = shader.get();
    cache.emplace_hint(iter, cache_key, std::move(shader));
}

#endif // __SWITCH__

MICROPROFILE_DECLARE(GPU_Shader);

void JitEngine::Run(const ShaderSetup& setup, ShaderUnit& state) const {
    if (setup.cached_shader == nullptr) {
        // This shader has no compiled code.
        interpreter->Run(setup, state);
        return;
    }

    MICROPROFILE_SCOPE(GPU_Shader);

    const JitShader* shader = static_cast<const JitShader*>(setup.cached_shader);
    shader->Run(setup, state, setup.entry_point);
}

} // namespace Pica::Shader

#endif // CITRA_ARCH(x86_64) || CITRA_ARCH(arm64)
