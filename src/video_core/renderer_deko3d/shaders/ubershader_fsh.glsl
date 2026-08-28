// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#version 460

// PICA fragment ubershader for the Deko3d backend.

#define NUM_TEV_STAGES 6
#define NUM_LIGHTS 8
#define NUM_LIGHTING_SAMPLERS 24

// Host depth-clip convention
#define DEPTH_MINUS_ONE_TO_ONE 1

// Matches FSUniformData in shader/generator/shader_uniforms.h.
struct LightSrc {
    vec3 specular_0;
    vec3 specular_1;
    vec3 diffuse;
    vec3 ambient;
    vec3 position;
    vec3 spot_direction;
    float dist_atten_bias;
    float dist_atten_scale;
};
layout (binding = 2, std140) uniform fs_data {
    int framebuffer_scale;
    int alphatest_ref;
    float depth_scale;
    float depth_offset;
    float shadow_bias_constant;
    float shadow_bias_linear;
    int scissor_x1;
    int scissor_y1;
    int scissor_x2;
    int scissor_y2;
    int fog_lut_offset;
    int proctex_noise_lut_offset;
    int proctex_color_map_offset;
    int proctex_alpha_map_offset;
    int proctex_lut_offset;
    int proctex_diff_lut_offset;
    float proctex_bias;
    int shadow_texture_bias;
    ivec4 lighting_lut_offset[NUM_LIGHTING_SAMPLERS / 4];
    vec3 fog_color;
    vec2 proctex_noise_f;
    vec2 proctex_noise_a;
    vec2 proctex_noise_p;
    vec3 lighting_global_ambient;
    LightSrc light_src[NUM_LIGHTS];
    vec4 const_color[NUM_TEV_STAGES];
    vec4 tev_combiner_buffer_color;
    vec3 tex_lod_bias;
    vec4 tex_border_color[3];
    vec4 blend_color;
};

// Configuration selectors
layout (binding = 0, std140) uniform fs_config {
    uint cfg_alpha_test_func;       // FramebufferRegs::CompareFunc
    uint cfg_scissor_mode;          // RasterizerRegs::ScissorMode
    uint cfg_depthmap_enable;       // 0 = Z-buffering, 1 = W-buffering
    uint cfg_texture0_type;         // TexturingRegs::TextureConfig::TextureType
    uint cfg_texture2_use_coord1;
    uint cfg_combiner_buffer_mask;  // tev_combiner_buffer_input (8 bits)
    uint cfg_fog_mode;              // TexturingRegs::FogMode
    uint cfg_fog_flip;
    uint cfg_lighting;              // LightConfig.raw
    uint cfg_proctex_enable;
    uint cfg_tex_border;            // 3 units x (enable_s | enable_t << 1)
    uint cfg_pad0;
    uvec4 cfg_tev[NUM_TEV_STAGES];  // x=sources y=modifiers z=ops w=scales
    uvec4 cfg_light[2];             // 8 packed Light.raw words
    uvec4 cfg_lut_raw[2];           // 7 packed LutConfig.raw words (d0,d1,sp,fr,rr,rg,rb)
    vec4 cfg_lut_scale[2];          // matching 7 LUT scales
};

// LUT slot indices into cfg_lut_raw / cfg_lut_scale.
#define LUT_D0 0
#define LUT_D1 1
#define LUT_SP 2
#define LUT_FR 3
#define LUT_RR 4
#define LUT_RG 5
#define LUT_RB 6

// LightingSampler enum values
#define SAMPLER_D0 0u
#define SAMPLER_D1 1u
#define SAMPLER_FR 3u
#define SAMPLER_RB 4u
#define SAMPLER_RG 5u
#define SAMPLER_RR 6u
#define SAMPLER_SP 8u
#define SAMPLER_DA 16u

// Interface from the trivial vertex shader.
layout (location = 1) in vec4 primary_color;
layout (location = 2) in vec2 texcoord0;
layout (location = 3) in vec2 texcoord1;
layout (location = 4) in vec2 texcoord2;
layout (location = 5) in float texcoord0_w;
layout (location = 6) in vec4 normquat;
layout (location = 7) in vec3 view;

layout (location = 0) out vec4 color;

// Texture samplers (binding 0-2), LUT texture buffers (3-5), cube view of tex0 (6).
layout (binding = 0) uniform sampler2D tex0;
layout (binding = 1) uniform sampler2D tex1;
layout (binding = 2) uniform sampler2D tex2;
layout (binding = 3) uniform samplerBuffer texture_buffer_lut_lf;
layout (binding = 4) uniform samplerBuffer texture_buffer_lut_rg;
layout (binding = 5) uniform samplerBuffer texture_buffer_lut_rgba;
layout (binding = 6) uniform samplerCube tex0_cube;

// State shared between the helper
vec4 g_rounded_primary_color;
vec4 g_primary_fragment_color;
vec4 g_secondary_fragment_color;
vec4 g_combiner_buffer;
vec4 g_next_combiner_buffer;
vec4 g_combiner_output;
float g_depth;

vec4 g_color_results_1;
vec4 g_color_results_2;
vec4 g_color_results_3;
float g_alpha_results_1;
float g_alpha_results_2;
float g_alpha_results_3;

// Lighting scratch.
vec3 g_normal;
vec3 g_tangent;
vec3 g_light_vector;
vec3 g_spot_dir;
vec3 g_half_vector;

// Generic helpers
vec3 quaternion_rotate(vec4 q, vec3 v) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

float byteround(float x) {
    return round(x * 255.0) * (1.0 / 255.0);
}
vec3 byteround(vec3 x) {
    return round(x * 255.0) * (1.0 / 255.0);
}
vec4 byteround(vec4 x) {
    return round(x * 255.0) * (1.0 / 255.0);
}

float getLod(vec2 coord) {
    vec2 d = max(abs(dFdx(coord)), abs(dFdy(coord)));
    return log2(max(d.x, d.y));
}

// Lighting LUT lookups.
float LookupLightingLUT(int lut_index, int index, float delta) {
    vec2 entry =
        texelFetch(texture_buffer_lut_lf, lighting_lut_offset[lut_index >> 2][lut_index & 3] + index).rg;
    return entry.r + entry.g * delta;
}

float LookupLightingLUTUnsigned(int lut_index, float pos) {
    int index = int(clamp(floor(pos * 256.0), 0.f, 255.f));
    float delta = pos * 256.0 - float(index);
    return LookupLightingLUT(lut_index, index, delta);
}

float LookupLightingLUTSigned(int lut_index, float pos) {
    int index = int(clamp(floor(pos * 128.0), -128.f, 127.f));
    float delta = pos * 128.0 - float(index);
    if (index < 0) index += 256;
    return LookupLightingLUT(lut_index, index, delta);
}

// Texture units.
vec4 sampleTexUnit0() {
    if (cfg_texture0_type == 5u) { // Disabled
        return vec4(0.0);
    }
    // Border colors apply to coordinate set 0 for unit 0.
    uint border = bitfieldExtract(cfg_tex_border, 0, 2);
    if ((border & 1u) != 0u && (texcoord0.x < 0.0 || texcoord0.x > 1.0)) {
        return tex_border_color[0];
    }
    if ((border & 2u) != 0u && (texcoord0.y < 0.0 || texcoord0.y > 1.0)) {
        return tex_border_color[0];
    }
    switch (cfg_texture0_type) {
    case 0u: // Texture2D
        return textureLod(tex0, texcoord0,
                          getLod(texcoord0 * vec2(textureSize(tex0, 0))) + tex_lod_bias[0]);
    case 3u: // Projection2D
        return textureProj(tex0, vec3(texcoord0, texcoord0_w));
    case 1u: // TextureCube
        return texture(tex0_cube, vec3(texcoord0, texcoord0_w));
    default: // Shadow2D / ShadowCube: deferred (D5 follow-up)
        return vec4(1.0);
    }
}

vec4 sampleTexUnit1() {
    return textureLod(tex1, texcoord1,
                      getLod(texcoord1 * vec2(textureSize(tex1, 0))) + tex_lod_bias[1]);
}

vec4 sampleTexUnit2() {
    uint border = bitfieldExtract(cfg_tex_border, 4, 2);
    vec2 uv = (cfg_texture2_use_coord1 != 0u) ? texcoord1 : texcoord2;
    if ((border & 1u) != 0u && (uv.x < 0.0 || uv.x > 1.0)) {
        return tex_border_color[2];
    }
    if ((border & 2u) != 0u && (uv.y < 0.0 || uv.y > 1.0)) {
        return tex_border_color[2];
    }
    return textureLod(tex2, uv, getLod(uv * vec2(textureSize(tex2, 0))) + tex_lod_bias[2]);
}

vec4 sampleTexUnit3() {
    // For now just disabled.
    return vec4(0.0);
}

vec4 sample_tex_unit(uint unit) {
    switch (unit) {
    case 0u: return sampleTexUnit0();
    case 1u: return sampleTexUnit1();
    case 2u: return sampleTexUnit2();
    default: return sampleTexUnit3();
    }
}

// Fragment lighting
bool lut_supported(uint kind, uint cfgL) {
    switch (kind) {
    case SAMPLER_D0: return cfgL != 1u;
    case SAMPLER_D1: return cfgL != 0u && cfgL != 1u && cfgL != 5u;
    case SAMPLER_SP: return cfgL != 2u && cfgL != 3u;
    case SAMPLER_FR: return cfgL != 0u && cfgL != 2u && cfgL != 4u;
    case SAMPLER_RR: return cfgL != 3u;
    case SAMPLER_RG:
    case SAMPLER_RB: return cfgL == 4u || cfgL == 5u || cfgL == 8u;
    default: return false;
    }
}

float lut_index_value(uint input_type, uint cfgL) {
    switch (input_type) {
    case 0u: return dot(g_normal, normalize(g_half_vector));        // NH
    case 1u: return dot(normalize(view), normalize(g_half_vector)); // VH
    case 2u: return dot(g_normal, normalize(view));                 // NV
    case 3u: return dot(g_light_vector, g_normal);                  // LN
    case 4u: return dot(g_light_vector, g_spot_dir);                // SP
    case 5u: {                                                      // CP (config 7 only)
        if (cfgL == 8u) {
            vec3 proj = normalize(g_half_vector) - g_normal * dot(g_normal, normalize(g_half_vector));
            return dot(proj, g_tangent);
        }
        return 0.0;
    }
    default: return 0.0;
    }
}

// Samples a lighting LUT.
float lut_lookup(uint lut_raw, float scale, uint sampler_index, bool two_sided, uint cfgL) {
    uint input_type = bitfieldExtract(lut_raw, 2, 3);
    bool abs_input = (lut_raw & 2u) != 0u;
    float idx = lut_index_value(input_type, cfgL);
    float value;
    if (abs_input) {
        idx = two_sided ? abs(idx) : max(idx, 0.0);
        value = LookupLightingLUTUnsigned(int(sampler_index), idx);
    } else {
        value = LookupLightingLUTSigned(int(sampler_index), idx);
    }
    return scale * value;
}

void do_lighting() {
    uint cfgL = bitfieldExtract(cfg_lighting, 11, 4);
    uint src_num = bitfieldExtract(cfg_lighting, 1, 4);
    uint bump_mode = bitfieldExtract(cfg_lighting, 5, 2);
    uint bump_selector = bitfieldExtract(cfg_lighting, 7, 2);
    bool bump_renorm = (cfg_lighting & (1u << 9)) != 0u;
    bool do_clamp_highlights = (cfg_lighting & (1u << 10)) != 0u;
    bool enable_primary_alpha = (cfg_lighting & (1u << 15)) != 0u;
    bool enable_secondary_alpha = (cfg_lighting & (1u << 16)) != 0u;

    vec4 diffuse_sum = vec4(0.0, 0.0, 0.0, 1.0);
    vec4 specular_sum = vec4(0.0, 0.0, 0.0, 1.0);

    // Surface-local normal/tangent from the bump mode.
    vec3 surface_normal;
    vec3 surface_tangent;
    if (bump_mode == 1u) { // NormalMap
        surface_normal = 2.0 * sample_tex_unit(bump_selector).rgb - 1.0;
        if (bump_renorm) {
            surface_normal.z = sqrt(max(
                1.0 - (surface_normal.x * surface_normal.x + surface_normal.y * surface_normal.y),
                0.0));
        }
        surface_tangent = vec3(1.0, 0.0, 0.0);
    } else if (bump_mode == 2u) { // TangentMap
        surface_tangent = 2.0 * sample_tex_unit(bump_selector).rgb - 1.0;
        surface_normal = vec3(0.0, 0.0, 1.0);
    } else {
        surface_normal = vec3(0.0, 0.0, 1.0);
        surface_tangent = vec3(1.0, 0.0, 0.0);
    }

    vec4 normalized_normquat = normalize(normquat);
    g_normal = quaternion_rotate(normalized_normquat, surface_normal);
    g_tangent = quaternion_rotate(normalized_normquat, surface_tangent);

    for (uint i = 0u; i < src_num; ++i) {
        uint light = cfg_light[i >> 2][i & 3u];
        uint num = bitfieldExtract(light, 0, 3);
        bool directional = (light & (1u << 3)) != 0u;
        bool two_sided = (light & (1u << 4)) != 0u;
        bool dist_atten_enable = (light & (1u << 5)) != 0u;
        bool spot_atten_enable = (light & (1u << 6)) != 0u;
        bool geo_factor_0 = (light & (1u << 7)) != 0u;
        bool geo_factor_1 = (light & (1u << 8)) != 0u;

        vec3 lpos = light_src[num].position;
        g_light_vector = directional ? lpos : lpos + view;
        float light_distance = length(g_light_vector);
        g_light_vector = normalize(g_light_vector);
        g_spot_dir = light_src[num].spot_direction;
        g_half_vector = normalize(view) + g_light_vector;

        float dot_product = two_sided ? abs(dot(g_light_vector, g_normal))
                                      : max(dot(g_light_vector, g_normal), 0.0);
        float clamp_highlights = do_clamp_highlights ? sign(dot_product) : 1.0;

        float spot_atten = 1.0;
        if (spot_atten_enable && lut_supported(SAMPLER_SP, cfgL)) {
            spot_atten = lut_lookup(cfg_lut_raw[LUT_SP >> 2][LUT_SP & 3], cfg_lut_scale[LUT_SP >> 2][LUT_SP & 3],
                                    SAMPLER_SP + num, two_sided, cfgL);
        }

        float dist_atten = 1.0;
        if (dist_atten_enable) {
            float idx = clamp(light_src[num].dist_atten_scale * light_distance +
                                  light_src[num].dist_atten_bias,
                              0.0, 1.0);
            dist_atten = LookupLightingLUTUnsigned(int(SAMPLER_DA + num), idx);
        }

        float geo_factor = 1.0;
        if (geo_factor_0 || geo_factor_1) {
            geo_factor = dot(g_half_vector, g_half_vector);
            geo_factor = geo_factor == 0.0 ? 0.0 : min(dot_product / geo_factor, 1.0);
        }

        float d0 = 1.0;
        if ((cfg_lut_raw[LUT_D0 >> 2][LUT_D0 & 3] & 1u) != 0u && lut_supported(SAMPLER_D0, cfgL)) {
            d0 = lut_lookup(cfg_lut_raw[LUT_D0 >> 2][LUT_D0 & 3], cfg_lut_scale[LUT_D0 >> 2][LUT_D0 & 3],
                            SAMPLER_D0, two_sided, cfgL);
        }
        vec3 specular_0 = d0 * light_src[num].specular_0;
        if (geo_factor_0) specular_0 *= geo_factor;

        vec3 refl_value;
        if ((cfg_lut_raw[LUT_RR >> 2][LUT_RR & 3] & 1u) != 0u && lut_supported(SAMPLER_RR, cfgL)) {
            refl_value.r = lut_lookup(cfg_lut_raw[LUT_RR >> 2][LUT_RR & 3], cfg_lut_scale[LUT_RR >> 2][LUT_RR & 3],
                                      SAMPLER_RR, two_sided, cfgL);
        } else {
            refl_value.r = 1.0;
        }
        if ((cfg_lut_raw[LUT_RG >> 2][LUT_RG & 3] & 1u) != 0u && lut_supported(SAMPLER_RG, cfgL)) {
            refl_value.g = lut_lookup(cfg_lut_raw[LUT_RG >> 2][LUT_RG & 3], cfg_lut_scale[LUT_RG >> 2][LUT_RG & 3],
                                      SAMPLER_RG, two_sided, cfgL);
        } else {
            refl_value.g = refl_value.r;
        }
        if ((cfg_lut_raw[LUT_RB >> 2][LUT_RB & 3] & 1u) != 0u && lut_supported(SAMPLER_RB, cfgL)) {
            refl_value.b = lut_lookup(cfg_lut_raw[LUT_RB >> 2][LUT_RB & 3], cfg_lut_scale[LUT_RB >> 2][LUT_RB & 3],
                                      SAMPLER_RB, two_sided, cfgL);
        } else {
            refl_value.b = refl_value.r;
        }

        float d1 = 1.0;
        if ((cfg_lut_raw[LUT_D1 >> 2][LUT_D1 & 3] & 1u) != 0u && lut_supported(SAMPLER_D1, cfgL)) {
            d1 = lut_lookup(cfg_lut_raw[LUT_D1 >> 2][LUT_D1 & 3], cfg_lut_scale[LUT_D1 >> 2][LUT_D1 & 3],
                            SAMPLER_D1, two_sided, cfgL);
        }
        vec3 specular_1 = d1 * refl_value * light_src[num].specular_1;
        if (geo_factor_1) specular_1 *= geo_factor;

        // Fresnel only applies on the last light slot.
        if (i == src_num - 1u && (cfg_lut_raw[LUT_FR >> 2][LUT_FR & 3] & 1u) != 0u &&
            lut_supported(SAMPLER_FR, cfgL)) {
            float fr = lut_lookup(cfg_lut_raw[LUT_FR >> 2][LUT_FR & 3], cfg_lut_scale[LUT_FR >> 2][LUT_FR & 3],
                                  SAMPLER_FR, two_sided, cfgL);
            if (enable_primary_alpha) diffuse_sum.a = fr;
            if (enable_secondary_alpha) specular_sum.a = fr;
        }

        diffuse_sum.rgb +=
            ((light_src[num].diffuse * dot_product) + light_src[num].ambient) * dist_atten * spot_atten;
        specular_sum.rgb += (specular_0 + specular_1) * clamp_highlights * dist_atten * spot_atten;
    }

    diffuse_sum.rgb += lighting_global_ambient;
    g_primary_fragment_color = clamp(diffuse_sum, vec4(0.0), vec4(1.0));
    g_secondary_fragment_color = clamp(specular_sum, vec4(0.0), vec4(1.0));
}

// TEV stages.
vec4 tev_source(uint source, int tev_index) {
    switch (source) {
    case 0x0u: return g_rounded_primary_color;
    case 0x1u: return g_primary_fragment_color;
    case 0x2u: return g_secondary_fragment_color;
    case 0x3u: return sampleTexUnit0();
    case 0x4u: return sampleTexUnit1();
    case 0x5u: return sampleTexUnit2();
    case 0x6u: return sampleTexUnit3();
    case 0xdu: return g_combiner_buffer;
    case 0xeu: return const_color[tev_index];
    case 0xfu: return g_combiner_output;
    default: return vec4(0.0);
    }
}

vec3 color_modifier(uint modifier, vec4 v) {
    switch (modifier) {
    case 0x0u: return v.rgb;
    case 0x1u: return vec3(1.0) - v.rgb;
    case 0x2u: return v.aaa;
    case 0x3u: return vec3(1.0) - v.aaa;
    case 0x4u: return v.rrr;
    case 0x5u: return vec3(1.0) - v.rrr;
    case 0x8u: return v.ggg;
    case 0x9u: return vec3(1.0) - v.ggg;
    case 0xcu: return v.bbb;
    case 0xdu: return vec3(1.0) - v.bbb;
    default: return vec3(0.0);
    }
}

float alpha_modifier(uint modifier, vec4 v) {
    switch (modifier) {
    case 0x0u: return v.a;
    case 0x1u: return 1.0 - v.a;
    case 0x2u: return v.r;
    case 0x3u: return 1.0 - v.r;
    case 0x4u: return v.g;
    case 0x5u: return 1.0 - v.g;
    case 0x6u: return v.b;
    case 0x7u: return 1.0 - v.b;
    default: return 0.0;
    }
}

vec3 color_combiner(uint op) {
    vec3 r1 = g_color_results_1.rgb;
    vec3 r2 = g_color_results_2.rgb;
    vec3 r3 = g_color_results_3.rgb;
    vec3 result;
    switch (op) {
    case 0u: result = r1; break;                                       // Replace
    case 1u: result = r1 * r2; break;                                  // Modulate
    case 2u: result = r1 + r2; break;                                  // Add
    case 3u: result = r1 + r2 - vec3(0.5); break;                      // AddSigned
    case 4u: result = mix(r2, r1, r3); break;                          // Lerp
    case 5u: result = r1 - r2; break;                                  // Subtract
    case 8u: result = fma(r1, r2, r3); break;                          // MultiplyThenAdd
    case 9u: result = min(r1 + r2, vec3(1.0)) * r3; break;             // AddThenMultiply
    case 6u:                                                           // Dot3_RGB
    case 7u: result = vec3(dot(r1 - vec3(0.5), r2 - vec3(0.5)) * 4.0); break; // Dot3_RGBA
    default: result = vec3(0.0); break;
    }
    return clamp(result, vec3(0.0), vec3(1.0));
}

float alpha_combiner(uint op) {
    float r1 = g_alpha_results_1;
    float r2 = g_alpha_results_2;
    float r3 = g_alpha_results_3;
    float result;
    switch (op) {
    case 0u: result = r1; break;
    case 1u: result = r1 * r2; break;
    case 2u: result = r1 + r2; break;
    case 3u: result = r1 + r2 - 0.5; break;
    case 4u: result = mix(r2, r1, r3); break;
    case 5u: result = r1 - r2; break;
    case 8u: result = fma(r1, r2, r3); break;
    case 9u: result = min(r1 + r2, 1.0) * r3; break;
    default: result = 0.0; break;
    }
    return clamp(result, 0.0, 1.0);
}

bool is_passthrough_stage(uint sources, uint modifiers, uint ops, uint scales) {
    uint color_op = bitfieldExtract(ops, 0, 4);
    uint alpha_op = bitfieldExtract(ops, 16, 4);
    uint color_src1 = bitfieldExtract(sources, 0, 4);
    uint alpha_src1 = bitfieldExtract(sources, 16, 4);
    uint color_mod1 = bitfieldExtract(modifiers, 0, 4);
    uint alpha_mod1 = bitfieldExtract(modifiers, 12, 3);
    uint color_scale = bitfieldExtract(scales, 0, 2);
    uint alpha_scale = bitfieldExtract(scales, 16, 2);
    return color_op == 0u && alpha_op == 0u && color_src1 == 0xfu && alpha_src1 == 0xfu &&
           color_mod1 == 0x0u && alpha_mod1 == 0x0u && color_scale == 0u && alpha_scale == 0u;
}

float scale_multiplier(uint scale) {
    return (scale < 3u) ? float(1u << scale) : 1.0;
}

void write_tev_stage(int index) {
    uint sources = cfg_tev[index].x;
    uint modifiers = cfg_tev[index].y;
    uint ops = cfg_tev[index].z;
    uint scales = cfg_tev[index].w;

    if (!is_passthrough_stage(sources, modifiers, ops, scales)) {
        uint color_src1 = bitfieldExtract(sources, 0, 4);
        uint color_src2 = bitfieldExtract(sources, 4, 4);
        uint color_src3 = bitfieldExtract(sources, 8, 4);
        uint alpha_src1 = bitfieldExtract(sources, 16, 4);
        uint alpha_src2 = bitfieldExtract(sources, 20, 4);
        uint alpha_src3 = bitfieldExtract(sources, 24, 4);

        // Previous reads color/alpha source3.
        if (index == 0) {
            if (color_src1 == 0xfu) color_src1 = color_src3;
            if (color_src2 == 0xfu) color_src2 = color_src3;
            if (alpha_src1 == 0xfu) alpha_src1 = alpha_src3;
            if (alpha_src2 == 0xfu) alpha_src2 = alpha_src3;
        }

        uint color_mod1 = bitfieldExtract(modifiers, 0, 4);
        uint color_mod2 = bitfieldExtract(modifiers, 4, 4);
        uint color_mod3 = bitfieldExtract(modifiers, 8, 4);
        uint alpha_mod1 = bitfieldExtract(modifiers, 12, 3);
        uint alpha_mod2 = bitfieldExtract(modifiers, 16, 3);
        uint alpha_mod3 = bitfieldExtract(modifiers, 20, 3);

        uint color_op = bitfieldExtract(ops, 0, 4);
        uint alpha_op = bitfieldExtract(ops, 16, 4);

        g_color_results_1.rgb = color_modifier(color_mod1, tev_source(color_src1, index));
        g_color_results_2.rgb = color_modifier(color_mod2, tev_source(color_src2, index));
        g_color_results_3.rgb = color_modifier(color_mod3, tev_source(color_src3, index));
        vec3 color_output = byteround(color_combiner(color_op));

        float alpha_output;
        if (color_op == 7u) { // Dot3_RGBA spills into alpha
            alpha_output = color_output[0];
        } else {
            g_alpha_results_1 = alpha_modifier(alpha_mod1, tev_source(alpha_src1, index));
            g_alpha_results_2 = alpha_modifier(alpha_mod2, tev_source(alpha_src2, index));
            g_alpha_results_3 = alpha_modifier(alpha_mod3, tev_source(alpha_src3, index));
            alpha_output = byteround(alpha_combiner(alpha_op));
        }

        float color_mult = scale_multiplier(bitfieldExtract(scales, 0, 2));
        float alpha_mult = scale_multiplier(bitfieldExtract(scales, 16, 2));
        g_combiner_output = vec4(clamp(color_output * color_mult, vec3(0.0), vec3(1.0)),
                                 clamp(alpha_output * alpha_mult, 0.0, 1.0));
    }

    g_combiner_buffer = g_next_combiner_buffer;
    if (index < 4 && (cfg_combiner_buffer_mask & (1u << uint(index))) != 0u) {
        g_next_combiner_buffer.rgb = g_combiner_output.rgb;
    }
    if (index < 4 && ((cfg_combiner_buffer_mask >> 4) & (1u << uint(index))) != 0u) {
        g_next_combiner_buffer.a = g_combiner_output.a;
    }
}

// main
void main() {
    g_rounded_primary_color = byteround(primary_color);
    g_primary_fragment_color = vec4(0.0);
    g_secondary_fragment_color = vec4(0.0);

    if (cfg_alpha_test_func == 0u) { // Never -> always fail
        discard;
    }

    // Scissor.
    if (cfg_scissor_mode != 0u) { // not Disabled
        bool inside = gl_FragCoord.x >= float(scissor_x1) && gl_FragCoord.y >= float(scissor_y1) &&
                      gl_FragCoord.x < float(scissor_x2) && gl_FragCoord.y < float(scissor_y2);
        // Include (3) keeps only pixels inside; Exclude (1) keeps only outside.
        if (cfg_scissor_mode == 3u ? !inside : inside) {
            discard;
        }
    }

    // Depth.
#if DEPTH_MINUS_ONE_TO_ONE
    float z_over_w = -2.0 * gl_FragCoord.z + 1.0;
#else
    float z_over_w = -gl_FragCoord.z;
#endif
    g_depth = z_over_w * depth_scale + depth_offset;
    if (cfg_depthmap_enable == 1u) { // W-buffering
        g_depth /= gl_FragCoord.w;
    }

    if ((cfg_lighting & 1u) != 0u) {
        do_lighting();
    }

    g_combiner_buffer = vec4(0.0);
    g_next_combiner_buffer = tev_combiner_buffer_color;
    g_combiner_output = vec4(0.0);
    g_color_results_1 = vec4(0.0);
    g_color_results_2 = vec4(0.0);
    g_color_results_3 = vec4(0.0);
    g_alpha_results_1 = 0.0;
    g_alpha_results_2 = 0.0;
    g_alpha_results_3 = 0.0;

    for (int i = 0; i < NUM_TEV_STAGES; ++i) {
        write_tev_stage(i);
    }

    // Alpha test.
    int a = int(g_combiner_output.a * 255.0);
    bool fail = false;
    switch (cfg_alpha_test_func) {
    case 2u: fail = (a != alphatest_ref); break; // Equal
    case 3u: fail = (a == alphatest_ref); break; // NotEqual
    case 4u: fail = (a >= alphatest_ref); break; // LessThan
    case 5u: fail = (a > alphatest_ref); break;  // LessThanOrEqual
    case 6u: fail = (a <= alphatest_ref); break; // GreaterThan
    case 7u: fail = (a < alphatest_ref); break;  // GreaterThanOrEqual
    default: fail = false; break;                // Always
    }
    if (fail) {
        discard;
    }

    // Fog.
    if (cfg_fog_mode == 5u) { // Fog
        float fog_index = (cfg_fog_flip != 0u) ? (1.0 - g_depth) * 128.0 : g_depth * 128.0;
        float fog_i = clamp(floor(fog_index), 0.0, 127.0);
        float fog_f = fog_index - fog_i;
        vec2 fog_lut_entry = texelFetch(texture_buffer_lut_lf, int(fog_i) + fog_lut_offset).rg;
        float fog_factor = clamp(fog_lut_entry.r + fog_lut_entry.g * fog_f, 0.0, 1.0);
        g_combiner_output.rgb = mix(fog_color.rgb, g_combiner_output.rgb, fog_factor);
    }

    gl_FragDepth = g_depth;
    g_combiner_output = byteround(g_combiner_output);
    color = g_combiner_output;
}
