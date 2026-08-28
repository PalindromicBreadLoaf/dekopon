// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deko3d.h>
#include "video_core/rasterizer_accelerated.h"
#include "video_core/renderer_deko3d/dk_shader.h"
#include "video_core/renderer_deko3d/dk_texture_runtime.h"

namespace Memory {
class MemorySystem;
}

namespace Pica {
class PicaCore;
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

    /// Records the immutable LUT buffer-texture and sampler descriptors once.
    void SetupStaticDescriptors();

    /// Syncs state, binds the ubershader and issues the batch.
    void Draw();
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
