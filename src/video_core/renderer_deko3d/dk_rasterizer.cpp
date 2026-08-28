// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>
#include "common/logging/log.h"
#include "common/vector_math.h"
#include "video_core/pica/pica_core.h"
#include "video_core/renderer_deko3d/dk_common.h"
#include "video_core/renderer_deko3d/dk_rasterizer.h"
#include "video_core/renderer_deko3d/pica_to_dk.h"
#include "video_core/shader/generator/pica_fs_config.h"

namespace Deko3D {

namespace {

constexpr u32 CmdBufSize = 512 * 1024;
constexpr u32 StreamBufferSize = 8 * 1024 * 1024;
constexpr u32 UniformBufferSize = 1 * 1024 * 1024;
constexpr u32 TextureBufferSize = 2 * 1024 * 1024;

using VideoCore::SurfaceType;

DkVtxAttribSize AttribSize(u32 components) {
    switch (components) {
    case 1:
        return DkVtxAttribSize_1x32;
    case 2:
        return DkVtxAttribSize_2x32;
    case 3:
        return DkVtxAttribSize_3x32;
    default:
        return DkVtxAttribSize_4x32;
    }
}

} // Anonymous namespace

void RasterizerDeko3D::StreamBuffer::Create(DkDevice device, u32 bytes) {
    size = AlignUp(bytes, DK_MEMBLOCK_ALIGNMENT);
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, size);
    maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    memblock = dkMemBlockCreate(&maker);
    cpu_addr = static_cast<u8*>(dkMemBlockGetCpuAddr(memblock));
    gpu_addr = dkMemBlockGetGpuAddr(memblock);
    head = 0;
    last_map = 0;
}

void RasterizerDeko3D::StreamBuffer::Destroy() {
    if (memblock != nullptr) {
        dkMemBlockDestroy(memblock);
        memblock = nullptr;
    }
}

u8* RasterizerDeko3D::StreamBuffer::Map(u32 bytes, u32 alignment, u32& out_offset,
                                       bool* out_wrapped) {
    head = AlignUp(head, alignment);
    bool wrapped = false;
    if (head + bytes > size) {
        head = 0;
        wrapped = true;
    }
    if (out_wrapped != nullptr) {
        *out_wrapped = wrapped;
    }
    last_map = head;
    out_offset = head;
    return cpu_addr + head;
}

void RasterizerDeko3D::StreamBuffer::Commit(u32 used) {
    head = last_map + used;
}

RasterizerDeko3D::RasterizerDeko3D(Memory::MemorySystem& memory, Pica::PicaCore& pica,
                                   VideoCore::CustomTexManager& custom_tex_manager,
                                   VideoCore::RendererBase& renderer, DkDevice device_,
                                   DkQueue queue_, TextureRuntime& runtime_)
    : VideoCore::RasterizerAccelerated{memory, pica}, device{device_}, queue{queue_},
      runtime{runtime_}, res_cache{memory, custom_tex_manager, runtime_, regs, renderer} {

    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, CmdBufSize);
    maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    cmdbuf_memblock = dkMemBlockCreate(&maker);

    DkCmdBufMaker cmdbuf_maker;
    dkCmdBufMakerDefaults(&cmdbuf_maker, device);
    cmdbuf = dkCmdBufCreate(&cmdbuf_maker);
    dkCmdBufAddMemory(cmdbuf, cmdbuf_memblock, 0, CmdBufSize);

    stream_buffer.Create(device, StreamBufferSize);
    uniform_buffer.Create(device, UniformBufferSize);
    texture_buffer.Create(device, TextureBufferSize);
    texture_lf_buffer.Create(device, TextureBufferSize);

    const u32 descriptor_size =
        AlignUp(NumImageDescriptors * static_cast<u32>(sizeof(DkImageDescriptor)) +
                    NumSamplerDescriptors * static_cast<u32>(sizeof(DkSamplerDescriptor)),
                DK_MEMBLOCK_ALIGNMENT);
    dkMemBlockMakerDefaults(&maker, device, descriptor_size);
    maker.flags = DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached;
    descriptor_memblock = dkMemBlockCreate(&maker);
    auto* const descriptor_base = static_cast<u8*>(dkMemBlockGetCpuAddr(descriptor_memblock));
    image_descriptors = reinterpret_cast<DkImageDescriptor*>(descriptor_base);
    sampler_descriptors = reinterpret_cast<DkSamplerDescriptor*>(
        descriptor_base + NumImageDescriptors * sizeof(DkImageDescriptor));
    image_descriptor_set = dkMemBlockGetGpuAddr(descriptor_memblock);
    sampler_descriptor_set =
        image_descriptor_set + NumImageDescriptors * sizeof(DkImageDescriptor);

    LoadUberShader();
    CreateNullResources();
    SetupStaticDescriptors();
}

RasterizerDeko3D::~RasterizerDeko3D() {
    if (queue != nullptr) {
        dkQueueWaitIdle(queue);
    }
    stream_buffer.Destroy();
    uniform_buffer.Destroy();
    texture_buffer.Destroy();
    texture_lf_buffer.Destroy();
    if (descriptor_memblock != nullptr) {
        dkMemBlockDestroy(descriptor_memblock);
    }
    if (code_memblock != nullptr) {
        dkMemBlockDestroy(code_memblock);
    }
    if (cmdbuf != nullptr) {
        dkCmdBufDestroy(cmdbuf);
    }
    if (cmdbuf_memblock != nullptr) {
        dkMemBlockDestroy(cmdbuf_memblock);
    }
}

void RasterizerDeko3D::LoadUberShader() {
    struct Spec {
        DkShader& shader;
        const char* path;
    };
    const std::array<Spec, 2> specs{{
        {uber_vsh, "romfs:/shaders/ubershader_vsh.dksh"},
        {uber_fsh, "romfs:/shaders/ubershader_fsh.dksh"},
    }};

    std::array<std::vector<u8>, 2> blobs{};
    std::array<u32, 2> offsets{};
    u32 total = 0;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        std::FILE* file = std::fopen(specs[i].path, "rb");
        if (file == nullptr) {
            LOG_ERROR(Render, "deko3d: could not open ubershader {}", specs[i].path);
            return;
        }
        std::fseek(file, 0, SEEK_END);
        const long fsize = std::ftell(file);
        std::rewind(file);
        if (fsize <= 0) {
            std::fclose(file);
            return;
        }
        blobs[i].resize(static_cast<std::size_t>(fsize));
        const std::size_t read = std::fread(blobs[i].data(), 1, blobs[i].size(), file);
        std::fclose(file);
        if (read != blobs[i].size()) {
            return;
        }
        offsets[i] = total;
        total += AlignUp(static_cast<u32>(fsize), DK_SHADER_CODE_ALIGNMENT);
    }

    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, device, AlignUp(total, DK_MEMBLOCK_ALIGNMENT));
    maker.flags =
        DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code;
    code_memblock = dkMemBlockCreate(&maker);
    auto* const base = static_cast<u8*>(dkMemBlockGetCpuAddr(code_memblock));
    for (std::size_t i = 0; i < specs.size(); ++i) {
        std::memcpy(base + offsets[i], blobs[i].data(), blobs[i].size());
        DkShaderMaker shader_maker;
        dkShaderMakerDefaults(&shader_maker, code_memblock, offsets[i]);
        dkShaderInitialize(&specs[i].shader, &shader_maker);
    }
    shaders_ok = true;
}

void RasterizerDeko3D::CreateNullResources() {
    const FormatTuple tuple = {DkImageFormat_RGBA8_Unorm,
                               DkImageFlags_UsageRender | DkImageFlags_Usage2DEngine};

    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, device);
    maker.flags = tuple.usage_flags;
    maker.format = tuple.format;
    maker.dimensions[0] = 1;
    maker.dimensions[1] = 1;
    DkImageLayout layout_2d;
    dkImageLayoutInitialize(&layout_2d, &maker);
    null_2d_alloc = runtime.ImagePool().Allocate(
        static_cast<u32>(dkImageLayoutGetSize(&layout_2d)), dkImageLayoutGetAlignment(&layout_2d));
    dkImageInitialize(&null_2d_image, &layout_2d, null_2d_alloc.MemBlock(), null_2d_alloc.Offset());

    dkImageLayoutMakerDefaults(&maker, device);
    maker.type = DkImageType_Cubemap;
    maker.flags = tuple.usage_flags;
    maker.format = tuple.format;
    maker.dimensions[0] = 1;
    maker.dimensions[1] = 1;
    DkImageLayout layout_cube;
    dkImageLayoutInitialize(&layout_cube, &maker);
    null_cube_alloc = runtime.ImagePool().Allocate(
        static_cast<u32>(dkImageLayoutGetSize(&layout_cube)),
        dkImageLayoutGetAlignment(&layout_cube));
    dkImageInitialize(&null_cube_image, &layout_cube, null_cube_alloc.MemBlock(),
                      null_cube_alloc.Offset());
}

void RasterizerDeko3D::SetupStaticDescriptors() {
    // Buffer-texture descriptors over the whole LUT streams.
    const auto make_buffer_desc = [&](u32 slot, const StreamBuffer& buffer, DkImageFormat format,
                                      u32 texel_bytes) {
        DkImageLayoutMaker maker;
        dkImageLayoutMakerDefaults(&maker, device);
        maker.type = DkImageType_Buffer;
        maker.format = format;
        maker.dimensions[0] = buffer.size / texel_bytes;
        DkImageLayout layout;
        dkImageLayoutInitialize(&layout, &maker);

        DkImage image;
        dkImageInitialize(&image, &layout, buffer.memblock, 0);
        DkImageView view;
        dkImageViewDefaults(&view, &image);
        dkImageDescriptorInitialize(&image_descriptors[slot], &view, false, false);
    };
    make_buffer_desc(3, texture_lf_buffer, DkImageFormat_RG32_Float, 8);
    make_buffer_desc(4, texture_buffer, DkImageFormat_RG32_Float, 8);
    make_buffer_desc(5, texture_buffer, DkImageFormat_RGBA32_Float, 16);

    // TODO: Wire cube textures
    DkImageView cube_view;
    dkImageViewDefaults(&cube_view, &null_cube_image);
    dkImageDescriptorInitialize(&image_descriptors[6], &cube_view, false, false);

    // Generic sampler used for the LUT buffers.
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    sampler.minFilter = DkFilter_Nearest;
    sampler.magFilter = DkFilter_Nearest;
    sampler.wrapMode[0] = DkWrapMode_ClampToEdge;
    sampler.wrapMode[1] = DkWrapMode_ClampToEdge;
    sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
    dkSamplerDescriptorInitialize(&sampler_descriptors[GenericSampler], &sampler);
}

void RasterizerDeko3D::TickFrame() {
    res_cache.TickFrame();
}

DkCmdBuf RasterizerDeko3D::BeginCmd() {
    dkCmdBufClear(cmdbuf);
    dkCmdBufAddMemory(cmdbuf, cmdbuf_memblock, 0, CmdBufSize);
    return cmdbuf;
}

void RasterizerDeko3D::SubmitCmd() {
    dkQueueSubmitCommands(queue, dkCmdBufFinishList(cmdbuf));
    dkQueueWaitIdle(queue);
}

void RasterizerDeko3D::SyncFixedState() {
    const bool is_flipped = regs.framebuffer.framebuffer.IsFlipped();

    dkRasterizerStateDefaults(&rasterizer_state);
    rasterizer_state.cullMode = PicaToDk::CullMode(regs.rasterizer.cull_mode, is_flipped);
    rasterizer_state.frontFace = PicaToDk::FrontFace(regs.rasterizer.cull_mode);

    const auto& output_merger = regs.framebuffer.output_merger;
    dkColorStateDefaults(&color_state);
    dkColorStateSetBlendEnable(&color_state, 0, output_merger.alphablend_enable != 0);
    color_state.logicOp = PicaToDk::LogicOp(output_merger.logic_op);

    const u32 color_mask = regs.framebuffer.framebuffer.allow_color_write != 0
                               ? (output_merger.depth_color_mask >> 8) & 0xF
                               : 0;
    dkColorWriteStateDefaults(&color_write_state);
    dkColorWriteStateSetMask(&color_write_state, 0, color_mask);

    dkBlendStateDefaults(&blend_state);
    const auto& blending = output_merger.alpha_blending;
    dkBlendStateSetOps(&blend_state, PicaToDk::BlendEquation(blending.blend_equation_rgb),
                       PicaToDk::BlendEquation(blending.blend_equation_a));
    dkBlendStateSetFactors(&blend_state, PicaToDk::BlendFunc(blending.factor_source_rgb),
                           PicaToDk::BlendFunc(blending.factor_dest_rgb),
                           PicaToDk::BlendFunc(blending.factor_source_a),
                           PicaToDk::BlendFunc(blending.factor_dest_a));
    blend_color = output_merger.blend_const.raw;

    const auto& stencil_test = output_merger.stencil_test;
    const bool stencil_enable = stencil_test.enable && regs.framebuffer.framebuffer.depth_format ==
                                                           Pica::FramebufferRegs::DepthFormat::D24S8;
    const bool depth_test_enabled =
        output_merger.depth_test_enable == 1 || output_merger.depth_write_enable == 1;
    const auto depth_compare = output_merger.depth_test_enable == 1
                                   ? output_merger.depth_test_func.Value()
                                   : Pica::FramebufferRegs::CompareFunc::Always;
    const bool depth_write = regs.framebuffer.framebuffer.allow_depth_stencil_write != 0 &&
                             output_merger.depth_write_enable != 0;

    dkDepthStencilStateDefaults(&depth_stencil_state);
    depth_stencil_state.depthTestEnable = depth_test_enabled;
    depth_stencil_state.depthWriteEnable = depth_write;
    depth_stencil_state.depthCompareOp = PicaToDk::CompareFunc(depth_compare);
    depth_stencil_state.stencilTestEnable = stencil_enable;
    depth_stencil_state.stencilFrontFailOp = PicaToDk::StencilOp(stencil_test.action_stencil_fail);
    depth_stencil_state.stencilFrontPassOp = PicaToDk::StencilOp(stencil_test.action_depth_pass);
    depth_stencil_state.stencilFrontDepthFailOp =
        PicaToDk::StencilOp(stencil_test.action_depth_fail);
    depth_stencil_state.stencilFrontCompareOp = PicaToDk::CompareFunc(stencil_test.func);
    depth_stencil_state.stencilBackFailOp = depth_stencil_state.stencilFrontFailOp;
    depth_stencil_state.stencilBackPassOp = depth_stencil_state.stencilFrontPassOp;
    depth_stencil_state.stencilBackDepthFailOp = depth_stencil_state.stencilFrontDepthFailOp;
    depth_stencil_state.stencilBackCompareOp = depth_stencil_state.stencilFrontCompareOp;

    stencil_reference = stencil_test.reference_value;
    stencil_compare_mask = stencil_test.input_mask;
    stencil_write_mask = regs.framebuffer.framebuffer.allow_depth_stencil_write != 0
                             ? static_cast<u8>(stencil_test.write_mask.Value())
                             : 0;
}

void RasterizerDeko3D::SyncTextureUnits() {
    const auto pica_textures = regs.texturing.GetTextures();
    for (u32 unit = 0; unit < pica_textures.size(); ++unit) {
        const auto& texture = pica_textures[unit];
        if (!texture.enabled) {
            DkImageView view;
            dkImageViewDefaults(&view, &null_2d_image);
            dkImageDescriptorInitialize(&image_descriptors[unit], &view, false, false);
            sampler_descriptors[unit] = sampler_descriptors[GenericSampler];
            continue;
        }

        Surface& surface = res_cache.GetTextureSurface(texture);
        Sampler& sampler = res_cache.GetSampler(texture.config);
        if (surface.Image() == nullptr) {
            DkImageView view;
            dkImageViewDefaults(&view, &null_2d_image);
            dkImageDescriptorInitialize(&image_descriptors[unit], &view, false, false);
        } else {
            const DkImageView view = surface.View();
            dkImageDescriptorInitialize(&image_descriptors[unit], &view, false, false);
        }
        sampler_descriptors[unit] = sampler.Descriptor();
    }
}

void RasterizerDeko3D::SyncAndUploadLUTsLF() {
    constexpr std::size_t max_size =
        sizeof(Common::Vec2f) * 256 * Pica::LightingRegs::NumLightingSampler +
        sizeof(Common::Vec2f) * 128; // fog

    if (!pica.lighting.lut_dirty && !pica.fog.lut_dirty) {
        return;
    }

    u32 offset = 0;
    bool wrapped = false;
    u8* buffer =
        texture_lf_buffer.Map(static_cast<u32>(max_size), sizeof(Common::Vec4f), offset, &wrapped);
    if (wrapped) {
        // The old texel offsets are about to be overwritten.
        pica.lighting.lut_dirty = pica.lighting.LutAllDirty;
        pica.fog.lut_dirty = true;
    }
    std::size_t bytes_used = 0;

    while (pica.lighting.lut_dirty) {
        const u32 index = std::countr_zero(pica.lighting.lut_dirty);
        pica.lighting.lut_dirty &= ~(1u << index);

        auto* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
        const auto& source_lut = pica.lighting.luts[index];
        for (u32 i = 0; i < source_lut.size(); ++i) {
            new_data[i] = {source_lut[i].ToFloat(), source_lut[i].DiffToFloat()};
        }
        fs_data.lighting_lut_offset[index / 4][index % 4] =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
        fs_data_dirty = true;
        bytes_used += source_lut.size() * sizeof(Common::Vec2f);
    }

    if (pica.fog.lut_dirty) {
        auto* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
        for (u32 i = 0; i < pica.fog.lut.size(); ++i) {
            new_data[i] = {pica.fog.lut[i].ToFloat(), pica.fog.lut[i].DiffToFloat()};
        }
        fs_data.fog_lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
        fs_data_dirty = true;
        bytes_used += pica.fog.lut.size() * sizeof(Common::Vec2f);
        pica.fog.lut_dirty = false;
    }

    texture_lf_buffer.Commit(static_cast<u32>(bytes_used));
}

void RasterizerDeko3D::SyncAndUploadLUTs() {
    const auto& proctex = pica.proctex;
    constexpr std::size_t max_size = sizeof(Common::Vec2f) * 128 * 3 + // noise + color + alpha
                                     sizeof(Common::Vec4f) * 256 +     // proctex
                                     sizeof(Common::Vec4f) * 256;      // proctex diff

    if (!pica.proctex.lut_dirty) {
        return;
    }

    u32 offset = 0;
    bool wrapped = false;
    u8* buffer =
        texture_buffer.Map(static_cast<u32>(max_size), sizeof(Common::Vec4f), offset, &wrapped);
    if (wrapped) {
        pica.proctex.table_dirty = pica.proctex.TableAllDirty;
    }
    std::size_t bytes_used = 0;

    const auto sync_value_lut =
        [&](const std::array<Pica::PicaCore::ProcTex::ValueEntry, 128>& lut, int& lut_offset) {
            auto* new_data = reinterpret_cast<Common::Vec2f*>(buffer + bytes_used);
            for (u32 i = 0; i < lut.size(); ++i) {
                new_data[i] = {lut[i].ToFloat(), lut[i].DiffToFloat()};
            }
            lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec2f));
            fs_data_dirty = true;
            bytes_used += lut.size() * sizeof(Common::Vec2f);
        };

    if (pica.proctex.noise_lut_dirty) {
        sync_value_lut(proctex.noise_table, fs_data.proctex_noise_lut_offset);
    }
    if (pica.proctex.color_map_dirty) {
        sync_value_lut(proctex.color_map_table, fs_data.proctex_color_map_offset);
    }
    if (pica.proctex.alpha_map_dirty) {
        sync_value_lut(proctex.alpha_map_table, fs_data.proctex_alpha_map_offset);
    }

    if (pica.proctex.lut_dirty) {
        auto* new_data = reinterpret_cast<Common::Vec4f*>(buffer + bytes_used);
        for (u32 i = 0; i < proctex.color_table.size(); ++i) {
            new_data[i] = proctex.color_table[i].ToVector() / 255.0f;
        }
        fs_data.proctex_lut_offset = static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
        fs_data_dirty = true;
        bytes_used += proctex.color_table.size() * sizeof(Common::Vec4f);
    }
    if (pica.proctex.diff_lut_dirty) {
        auto* new_data = reinterpret_cast<Common::Vec4f*>(buffer + bytes_used);
        for (u32 i = 0; i < proctex.color_diff_table.size(); ++i) {
            new_data[i] = proctex.color_diff_table[i].ToVector() / 255.0f;
        }
        fs_data.proctex_diff_lut_offset =
            static_cast<int>((offset + bytes_used) / sizeof(Common::Vec4f));
        fs_data_dirty = true;
        bytes_used += proctex.color_diff_table.size() * sizeof(Common::Vec4f);
    }

    pica.proctex.table_dirty = 0;
    texture_buffer.Commit(static_cast<u32>(bytes_used));
}

void RasterizerDeko3D::DrawTriangles() {
    if (vertex_batch.empty()) {
        return;
    }
    Draw();
    vertex_batch.clear();
}

void RasterizerDeko3D::Draw() {
    if (!shaders_ok) {
        return;
    }

    SyncDrawUniforms();
    SyncFixedState();

    const bool shadow_rendering = regs.framebuffer.IsShadowRendering();
    const bool has_stencil = regs.framebuffer.HasStencil();

    const u32 color_mask = regs.framebuffer.framebuffer.allow_color_write != 0
                               ? (regs.framebuffer.output_merger.depth_color_mask >> 8) & 0xF
                               : 0;
    const bool write_color_fb = shadow_rendering || color_mask != 0;
    const bool write_depth_fb =
        regs.framebuffer.framebuffer.allow_depth_stencil_write != 0 &&
        regs.framebuffer.output_merger.depth_write_enable != 0;
    const bool using_color_fb =
        regs.framebuffer.framebuffer.GetColorBufferPhysicalAddress() != 0 && write_color_fb;
    const bool using_depth_fb =
        !shadow_rendering && regs.framebuffer.framebuffer.GetDepthBufferPhysicalAddress() != 0 &&
        (write_depth_fb || regs.framebuffer.output_merger.depth_test_enable != 0 ||
         (has_stencil && depth_stencil_state.stencilTestEnable));

    auto fb_helper = res_cache.GetFramebufferSurfaces(using_color_fb, using_depth_fb);
    Framebuffer* framebuffer = fb_helper.Framebuffer();
    const bool has_color = framebuffer->HasAttachment(SurfaceType::Color);
    const bool has_depth = framebuffer->HasAttachment(SurfaceType::Depth);
    if (!has_color && !has_depth) {
        return;
    }

    const auto scissor = fb_helper.Scissor();
    fs_data.scissor_x1 = scissor.left;
    fs_data.scissor_x2 = scissor.right;
    fs_data.scissor_y1 = scissor.bottom;
    fs_data.scissor_y2 = scissor.top;
    fs_data_dirty = true;

    SyncTextureUnits();
    SyncAndUploadLUTs();
    SyncAndUploadLUTsLF();

    // fs_config selector block
    const Pica::Shader::FSConfig fs_config{regs};
    const FSConfigUniformData fs_config_data = BuildFSConfigUniform(fs_config);

    const auto upload_uniform = [&](const void* data, u32 bytes) -> DkGpuAddr {
        const u32 aligned = AlignUp(bytes, DK_UNIFORM_BUF_ALIGNMENT);
        u32 offset = 0;
        u8* ptr = uniform_buffer.Map(aligned, DK_UNIFORM_BUF_ALIGNMENT, offset);
        std::memcpy(ptr, data, bytes);
        uniform_buffer.Commit(aligned);
        return uniform_buffer.gpu_addr + offset;
    };
    const DkGpuAddr fs_config_addr = upload_uniform(&fs_config_data, sizeof(fs_config_data));
    const DkGpuAddr vs_data_addr = upload_uniform(&vs_data, sizeof(vs_data));
    const DkGpuAddr fs_data_addr = upload_uniform(&fs_data, sizeof(fs_data));

    // Vertex batch.
    const u32 vertex_count = static_cast<u32>(vertex_batch.size());
    const u32 vertex_bytes = vertex_count * sizeof(HardwareVertex);
    u32 vertex_offset = 0;
    u8* vertex_ptr = stream_buffer.Map(vertex_bytes, sizeof(HardwareVertex), vertex_offset);
    std::memcpy(vertex_ptr, vertex_batch.data(), vertex_bytes);
    stream_buffer.Commit(vertex_bytes);
    const DkGpuAddr vertex_addr = stream_buffer.gpu_addr + vertex_offset;

    // Record
    DkCmdBuf cb = BeginCmd();

    DkImageView color_view;
    DkImageView depth_view;
    const DkImageView* color_targets[1] = {nullptr};
    const DkImageView* depth_ptr = nullptr;
    if (has_color) {
        dkImageViewDefaults(&color_view, framebuffer->Image(SurfaceType::Color));
        color_targets[0] = &color_view;
    }
    if (has_depth) {
        dkImageViewDefaults(&depth_view, framebuffer->Image(SurfaceType::Depth));
        depth_ptr = &depth_view;
    }
    dkCmdBufBindRenderTargets(cb, color_targets, has_color ? 1 : 0, depth_ptr);

    const auto viewport_info = fb_helper.Viewport();
    const DkViewport viewport = {static_cast<float>(viewport_info.x),
                                 static_cast<float>(viewport_info.y),
                                 static_cast<float>(viewport_info.width),
                                 static_cast<float>(viewport_info.height),
                                 0.0f,
                                 1.0f};
    const auto draw_rect = fb_helper.DrawRect();
    const DkScissor scissor_rect = {draw_rect.left, draw_rect.bottom, draw_rect.GetWidth(),
                                    draw_rect.GetHeight()};
    dkCmdBufSetViewports(cb, 0, &viewport, 1);
    dkCmdBufSetScissors(cb, 0, &scissor_rect, 1);

    dkCmdBufBarrier(cb, DkBarrier_Full,
                    DkInvalidateFlags_Image | DkInvalidateFlags_Descriptors);

    dkCmdBufBindRasterizerState(cb, &rasterizer_state);
    dkCmdBufBindColorState(cb, &color_state);
    dkCmdBufBindColorWriteState(cb, &color_write_state);
    dkCmdBufBindBlendStates(cb, 0, &blend_state, 1);
    dkCmdBufBindDepthStencilState(cb, &depth_stencil_state);
    dkCmdBufSetStencil(cb, DkFace_FrontAndBack, stencil_write_mask, stencil_reference,
                       stencil_compare_mask);
    const Common::Vec4f blend_rgba = PicaToDk::ColorRGBA8(blend_color);
    dkCmdBufSetBlendConst(cb, blend_rgba.r(), blend_rgba.g(), blend_rgba.b(), blend_rgba.a());

    const DkShader* shaders[2] = {&uber_vsh, &uber_fsh};
    dkCmdBufBindShaders(cb, DkStageFlag_GraphicsMask, shaders, 2);

    dkCmdBufBindUniformBuffer(cb, DkStage_Vertex, 1, vs_data_addr,
                              AlignUp(sizeof(vs_data), DK_UNIFORM_BUF_ALIGNMENT));
    dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 0, fs_config_addr,
                              AlignUp(sizeof(fs_config_data), DK_UNIFORM_BUF_ALIGNMENT));
    dkCmdBufBindUniformBuffer(cb, DkStage_Fragment, 2, fs_data_addr,
                              AlignUp(sizeof(fs_data), DK_UNIFORM_BUF_ALIGNMENT));

    dkCmdBufBindImageDescriptorSet(cb, image_descriptor_set, NumImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(cb, sampler_descriptor_set, NumSamplerDescriptors);
    const std::array<DkResHandle, NumImageDescriptors> handles = {
        dkMakeTextureHandle(0, 0),
        dkMakeTextureHandle(1, 1),
        dkMakeTextureHandle(2, 2),
        dkMakeTextureHandle(3, GenericSampler),
        dkMakeTextureHandle(4, GenericSampler),
        dkMakeTextureHandle(5, GenericSampler),
        dkMakeTextureHandle(6, 0),
    };
    dkCmdBufBindTextures(cb, DkStage_Fragment, 0, handles.data(), handles.size());

    static constexpr std::array<std::pair<u32, u32>, 8> attrib_layout = {{
        {offsetof(HardwareVertex, position), 4},   {offsetof(HardwareVertex, color), 4},
        {offsetof(HardwareVertex, tex_coord0), 2}, {offsetof(HardwareVertex, tex_coord1), 2},
        {offsetof(HardwareVertex, tex_coord2), 2}, {offsetof(HardwareVertex, tex_coord0_w), 1},
        {offsetof(HardwareVertex, normquat), 4},   {offsetof(HardwareVertex, view), 3},
    }};
    std::array<DkVtxAttribState, 8> attribs{};
    for (u32 i = 0; i < attribs.size(); ++i) {
        attribs[i] = {0, 0, attrib_layout[i].first, AttribSize(attrib_layout[i].second),
                      DkVtxAttribType_Float, 0};
    }
    const DkVtxBufferState buffer_state = {sizeof(HardwareVertex), 0};
    dkCmdBufBindVtxAttribState(cb, attribs.data(), attribs.size());
    dkCmdBufBindVtxBufferState(cb, &buffer_state, 1);
    dkCmdBufBindVtxBuffer(cb, 0, vertex_addr, vertex_bytes);
    dkCmdBufDraw(cb, DkPrimitive_Triangles, vertex_count, 1, 0, 0);

    SubmitCmd();
}

void RasterizerDeko3D::FlushAll() {
    res_cache.FlushAll();
}

void RasterizerDeko3D::FlushRegion(PAddr addr, u32 size) {
    res_cache.FlushRegion(addr, size);
}

void RasterizerDeko3D::InvalidateRegion(PAddr addr, u32 size) {
    res_cache.InvalidateRegion(addr, size);
}

void RasterizerDeko3D::FlushAndInvalidateRegion(PAddr addr, u32 size) {
    res_cache.FlushRegion(addr, size);
    res_cache.InvalidateRegion(addr, size);
}

void RasterizerDeko3D::ClearAll(bool flush) {
    res_cache.ClearAll(flush);
}

} // namespace Deko3D
