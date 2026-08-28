// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <optional>
#include <unordered_map>
#include <vector>
#include <deko3d.h>
#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_deko3d/dk_shader.h"
#include "video_core/renderer_deko3d/dk_texture_runtime.h"
#include "video_core/shader/generator/profile.h"

namespace Memory {
class MemorySystem;
}

namespace Pica {
class PicaCore;
}

namespace Pica::Shader {
struct FSConfig;
}

namespace VideoCore {
class CustomTexManager;
class RendererBase;
} // namespace VideoCore

namespace Deko3D {

// Deko3d rasterizer.
class RasterizerDeko3D : public VideoCore::RasterizerAccelerated {
public:
    explicit RasterizerDeko3D(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                              VideoCore::CustomTexManager& custom_tex_manager,
                              VideoCore::RendererBase& renderer, DkDevice device, DkQueue queue,
                              TextureRuntime& runtime);
    ~RasterizerDeko3D() override;

    void TickFrame();

    void DrawTriangles() override;
    bool AccelerateDrawBatch(bool is_indexed) override;
    void FlushAll() override;
    void FlushRegion(PAddr addr, u32 size) override;
    void InvalidateRegion(PAddr addr, u32 size) override;
    void FlushAndInvalidateRegion(PAddr addr, u32 size) override;
    void ClearAll(bool flush) override;

private:
    /// A CPU-visible ring buffer handed out in aligned slices via Map/Commit.
    struct StreamBuffer {
        DkMemBlock memblock = nullptr;
        u8* cpu_addr = nullptr;
        DkGpuAddr gpu_addr = 0;
        u32 size = 0;
        u32 head = 0;
        u32 last_map = 0;

        void Create(DkDevice device, u32 bytes);
        void Destroy();
        /// Reserves `bytes` aligned to `alignment`.
        u8* Map(u32 bytes, u32 alignment, u32& out_offset, bool* out_wrapped = nullptr);
        /// Advances the head past the actually-used byte count of the last Map.
        void Commit(u32 used);
    };

    void LoadUberShader();
    void CreateNullResources();

    /// Returns the specialised fragment shader for the current PICA state.
    const DkShader* GetFragmentShader();
    /// Decompiles the current PICA vertex shader to a DKSH.
    const DkShader* GetVertexShader();
    /// Bump-allocates a DKSH blob into the code arena and initialises `out`.
    bool UploadShaderCode(const std::vector<u8>& dksh, DkShader& out);
    /// Whether the generated fragment shader only touches bindings the rasterizer already provides.
    static bool CanSpecialize(const Pica::Shader::FSConfig& config);

    /// Streams the raw PICA vertex arrays and builds the deko3d attribute/buffer state.
    bool SetupVertexArray();
    /// Assigns fixed/default values to attributes not sourced from a loader.
    void SetupFixedAttribs();
    /// Streams the index array for an accelerated indexed draw.
    void SetupIndexArray();

    /// Records the immutable LUT buffer-texture and sampler descriptors once.
    void SetupStaticDescriptors();

    /// Syncs state, binds the shaders and issues the batch.
    void Draw(bool accelerate, bool is_indexed);
    /// Fills the fixed-function state objects from PICA registers.
    void SyncFixedState();
    /// Binds the three PICA texture units into the descriptor set.
    void SyncTextureUnits();
    /// Streams the fs_config/vs_data/fs_data uniform blocks.
    void UploadUniforms();
    /// Streams the lighting/fog and proctex LUTs into the texel buffers.
    void SyncAndUploadLUTs();
    void SyncAndUploadLUTsLF();

    DkCmdBuf BeginCmd();
    void SubmitCmd();

private:
    DkDevice device;
    DkQueue queue;
    TextureRuntime& runtime;
    RasterizerCache res_cache;

    DkMemBlock cmdbuf_memblock = nullptr;
    DkCmdBuf cmdbuf = nullptr;

    DkMemBlock code_memblock = nullptr;
    DkShader uber_vsh{};
    DkShader uber_fsh{};
    bool shaders_ok = false;

    Pica::Shader::Profile fs_profile{};
    // Specialised fragment shaders keyed by canonical FSConfig hash.
    std::unordered_map<u64, std::optional<DkShader>> fragment_shaders;
    // Decompiled vertex shaders keyed by PicaVSConfig hash.
    std::unordered_map<u64, std::optional<DkShader>> vertex_shaders;
    std::vector<DkMemBlock> shader_code_arena;
    DkMemBlock shader_code_block = nullptr;
    u32 shader_code_capacity = 0;
    u32 shader_code_head = 0;

    // Accelerated draw scratch state
    static constexpr u32 MaxVertexBindings = 16;
    VertexArrayInfo vertex_info{};
    const DkShader* accel_vsh = nullptr;
    DkPrimitive accel_topology = DkPrimitive_Triangles;
    std::array<DkVtxAttribState, 16> accel_attribs{};
    std::array<DkVtxBufferState, MaxVertexBindings> accel_buffers{};
    std::array<DkGpuAddr, MaxVertexBindings> accel_buffer_addrs{};
    std::array<u32, MaxVertexBindings> accel_buffer_sizes{};
    std::array<bool, 16> accel_enabled_attribs{};
    u32 accel_binding_count = 0;
    DkGpuAddr accel_index_addr = 0;
    DkIdxFormat accel_index_format = DkIdxFormat_Uint16;

    StreamBuffer stream_buffer;     /// Vertex data
    StreamBuffer uniform_buffer;    /// fs_config / vs_data / fs_data
    StreamBuffer texture_buffer;    /// proctex LUTs
    StreamBuffer texture_lf_buffer; /// lighting + fog LUTs

    // Image descriptor slots. 0-2 texture units, 3-5 LUT buffers, 6 tex0 cube.
    static constexpr u32 NumImageDescriptors = 7;
    // Sampler slots. 0-2 per texture unit, 3 generic.
    static constexpr u32 NumSamplerDescriptors = 4;
    static constexpr u32 GenericSampler = 3;

    DkMemBlock descriptor_memblock = nullptr;
    DkGpuAddr image_descriptor_set = 0;
    DkGpuAddr sampler_descriptor_set = 0;
    DkImageDescriptor* image_descriptors = nullptr;
    DkSamplerDescriptor* sampler_descriptors = nullptr;

    MemoryAllocation null_2d_alloc;
    MemoryAllocation null_cube_alloc;
    DkImage null_2d_image{};
    DkImage null_cube_image{};

    DkRasterizerState rasterizer_state{};
    DkColorState color_state{};
    DkColorWriteState color_write_state{};
    DkBlendState blend_state{};
    DkDepthStencilState depth_stencil_state{};
    DkPrimitive topology = DkPrimitive_Triangles;
    u32 blend_color = 0;
    u8 stencil_reference = 0;
    u8 stencil_compare_mask = 0xFF;
    u8 stencil_write_mask = 0xFF;
};

} // namespace Deko3D
