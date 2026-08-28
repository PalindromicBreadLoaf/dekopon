// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <mutex>
#include "common/logging/log.h"
#include "video_core/renderer_deko3d/dk_shader_compiler.h"
#include "video_core/renderer_deko3d/uam_bridge.h"

namespace Deko3D::ShaderCompiler {
namespace {

int StageToUam(Stage stage) {
    switch (stage) {
    case Stage::Vertex:
        return UamStage_Vertex;
    case Stage::Geometry:
        return UamStage_Geometry;
    case Stage::Fragment:
        return UamStage_Fragment;
    case Stage::Compute:
        return UamStage_Compute;
    }
    return UamStage_Vertex;
}

// uam is mesa 19.0's single threaded standalone compiler.
std::mutex s_compile_mutex;

} // namespace

std::optional<std::vector<u8>> CompileShader(Stage stage, std::string_view source,
                                             std::string_view name) {
    const std::string full_source{source};

    const auto start = std::chrono::steady_clock::now();

    size_t dksh_size = 0;
    char* log = nullptr;
    void* dksh = nullptr;
    {
        std::lock_guard guard(s_compile_mutex);
        dksh = UamCompileGlsl(full_source.c_str(), StageToUam(stage), &dksh_size, &log);
    }

    const bool have_log = log != nullptr && log[0] != '\0';
    if (!dksh) {
        LOG_ERROR(Render, "deko3d: uam rejected '{}':\n{}", name,
                  have_log ? log : "(no diagnostics)");
        std::free(log);
        return std::nullopt;
    }

    if (have_log) {
        LOG_WARNING(Render, "deko3d: uam warnings for '{}':\n{}", name, log);
    }
    std::free(log);

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    LOG_DEBUG(Render, "deko3d: compiled '{}' to {} bytes of DKSH in {} ms", name, dksh_size, ms);

    const auto* bytes = static_cast<const u8*>(dksh);
    std::vector<u8> blob(bytes, bytes + dksh_size);
    std::free(dksh);
    return blob;
}

} // namespace Deko3D::ShaderCompiler
