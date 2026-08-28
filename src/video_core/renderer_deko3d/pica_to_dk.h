// SPDX-FileCopyrightText: Azahar Emulator Project
// Copyright(c) 2026: PalindromicBreadLoaf(palindromicbreadloaf@tuta.com)
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <deko3d.h>
#include "common/vector_math.h"
#include "video_core/pica/regs_internal.h"

namespace PicaToDk {

inline Common::Vec4f ColorRGBA8(const u32 color) {
    const auto rgba =
        Common::Vec4u{color >> 0 & 0xFF, color >> 8 & 0xFF, color >> 16 & 0xFF, color >> 24 & 0xFF};
    return rgba / 255.0f;
}

inline DkCompareOp CompareFunc(Pica::FramebufferRegs::CompareFunc func) {
    static constexpr std::array<DkCompareOp, 8> table{{
        DkCompareOp_Never,    // Never
        DkCompareOp_Always,   // Always
        DkCompareOp_Equal,    // Equal
        DkCompareOp_NotEqual, // NotEqual
        DkCompareOp_Less,     // LessThan
        DkCompareOp_Lequal,   // LessThanOrEqual
        DkCompareOp_Greater,  // GreaterThan
        DkCompareOp_Gequal,   // GreaterThanOrEqual
    }};
    const auto index = static_cast<std::size_t>(func);
    return index < table.size() ? table[index] : DkCompareOp_Always;
}

inline DkBlendOp BlendEquation(Pica::FramebufferRegs::BlendEquation equation) {
    static constexpr std::array<DkBlendOp, 5> table{{
        DkBlendOp_Add,    // Add
        DkBlendOp_Sub,    // Subtract
        DkBlendOp_RevSub, // ReverseSubtract
        DkBlendOp_Min,    // Min
        DkBlendOp_Max,    // Max
    }};
    const auto index = static_cast<std::size_t>(equation);
    return index < table.size() ? table[index] : DkBlendOp_Add;
}

inline DkBlendFactor BlendFunc(Pica::FramebufferRegs::BlendFactor factor) {
    static constexpr std::array<DkBlendFactor, 15> table{{
        DkBlendFactor_Zero,             // Zero
        DkBlendFactor_One,              // One
        DkBlendFactor_SrcColor,         // SourceColor
        DkBlendFactor_InvSrcColor,      // OneMinusSourceColor
        DkBlendFactor_DstColor,         // DestColor
        DkBlendFactor_InvDstColor,      // OneMinusDestColor
        DkBlendFactor_SrcAlpha,         // SourceAlpha
        DkBlendFactor_InvSrcAlpha,      // OneMinusSourceAlpha
        DkBlendFactor_DstAlpha,         // DestAlpha
        DkBlendFactor_InvDstAlpha,      // OneMinusDestAlpha
        DkBlendFactor_ConstColor,       // ConstantColor
        DkBlendFactor_InvConstColor,    // OneMinusConstantColor
        DkBlendFactor_ConstAlpha,       // ConstantAlpha
        DkBlendFactor_InvConstAlpha,    // OneMinusConstantAlpha
        DkBlendFactor_SrcAlphaSaturate, // SourceAlphaSaturate
    }};
    const auto index = static_cast<std::size_t>(factor);
    return index < table.size() ? table[index] : DkBlendFactor_One;
}

inline DkLogicOp LogicOp(Pica::FramebufferRegs::LogicOp op) {
    static constexpr std::array<DkLogicOp, 16> table{{
        DkLogicOp_Clear,        // Clear
        DkLogicOp_And,          // And
        DkLogicOp_AndReverse,   // AndReverse
        DkLogicOp_Copy,         // Copy
        DkLogicOp_Set,          // Set
        DkLogicOp_CopyInverted, // CopyInverted
        DkLogicOp_NoOp,         // NoOp
        DkLogicOp_Invert,       // Invert
        DkLogicOp_Nand,         // Nand
        DkLogicOp_Or,           // Or
        DkLogicOp_Nor,          // Nor
        DkLogicOp_Xor,          // Xor
        DkLogicOp_Equivalent,   // Equiv
        DkLogicOp_AndInverted,  // AndInverted
        DkLogicOp_OrReverse,    // OrReverse
        DkLogicOp_OrInverted,   // OrInverted
    }};
    const auto index = static_cast<std::size_t>(op);
    return index < table.size() ? table[index] : DkLogicOp_Copy;
}

inline DkStencilOp StencilOp(Pica::FramebufferRegs::StencilAction action) {
    static constexpr std::array<DkStencilOp, 8> table{{
        DkStencilOp_Keep,     // Keep
        DkStencilOp_Zero,     // Zero
        DkStencilOp_Replace,  // Replace
        DkStencilOp_Incr,     // Increment
        DkStencilOp_Decr,     // Decrement
        DkStencilOp_Invert,   // Invert
        DkStencilOp_IncrWrap, // IncrementWrap
        DkStencilOp_DecrWrap, // DecrementWrap
    }};
    const auto index = static_cast<std::size_t>(action);
    return index < table.size() ? table[index] : DkStencilOp_Keep;
}

inline DkFace CullMode(Pica::RasterizerRegs::CullMode mode, bool flip_viewport) {
    switch (mode) {
    case Pica::RasterizerRegs::CullMode::KeepAll:
        return DkFace_None;
    case Pica::RasterizerRegs::CullMode::KeepClockWise:
    case Pica::RasterizerRegs::CullMode::KeepCounterClockWise:
        return flip_viewport ? DkFace_Front : DkFace_Back;
    default:
        return DkFace_None;
    }
}

inline DkFrontFace FrontFace(Pica::RasterizerRegs::CullMode mode) {
    switch (mode) {
    case Pica::RasterizerRegs::CullMode::KeepAll:
    case Pica::RasterizerRegs::CullMode::KeepClockWise:
        return DkFrontFace_CCW;
    case Pica::RasterizerRegs::CullMode::KeepCounterClockWise:
        return DkFrontFace_CW;
    default:
        return DkFrontFace_CCW;
    }
}

inline DkPrimitive PrimitiveTopology(Pica::PipelineRegs::TriangleTopology topology) {
    switch (topology) {
    case Pica::PipelineRegs::TriangleTopology::Fan:
        return DkPrimitive_TriangleFan;
    case Pica::PipelineRegs::TriangleTopology::Strip:
        return DkPrimitive_TriangleStrip;
    case Pica::PipelineRegs::TriangleTopology::List:
    case Pica::PipelineRegs::TriangleTopology::Shader:
    default:
        return DkPrimitive_Triangles;
    }
}

} // namespace PicaToDk
