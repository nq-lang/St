#pragma once
// theme.hpp — color constants lifted 1:1 from the dark terminal reference.
// Kept out of main.cpp only because it's pure data (no logic) and long;
// everything else lives in main.cpp.

#include "imgui.h"
#include <cstdint>

namespace theme {

constexpr ImVec4 rgb(uint32_t hex, float alpha = 1.0f) noexcept {
    return ImVec4(
        static_cast<float>((hex >> 16) & 0xFF) / 255.0f,
        static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
        static_cast<float>(hex & 0xFF) / 255.0f,
        alpha);
}

inline constexpr uint32_t kBgBody      = 0x0b0d12;
inline constexpr uint32_t kBgPanel     = 0x11151d;
inline constexpr uint32_t kBorder      = 0x1f2937;
inline constexpr uint32_t kTextPrimary = 0xd1d5db;
inline constexpr uint32_t kTextMuted   = 0x9ca3af;
inline constexpr uint32_t kTextDim     = 0x6b7280;
inline constexpr uint32_t kGridLine    = 0x374151;

inline constexpr uint32_t kYellow  = 0xeab308;
inline constexpr uint32_t kGreen   = 0x22c55e;
inline constexpr uint32_t kRed     = 0xef4444;
inline constexpr uint32_t kBlue    = 0x3b82f6;
inline constexpr uint32_t kOrange  = 0xf97316;
inline constexpr uint32_t kCyan    = 0x38bdf8;
inline constexpr uint32_t kPurple  = 0xa855f7;
inline constexpr uint32_t kEmerald = 0x10b981;

inline constexpr uint32_t kHeatB1 = 0x1e3a8a;
inline constexpr uint32_t kHeatB2 = 0x2563eb;
inline constexpr uint32_t kHeatB3 = 0x3b82f6;
inline constexpr uint32_t kHeatG1 = 0x10b981;
inline constexpr uint32_t kHeatG2 = 0x22c55e;
inline constexpr uint32_t kHeatY1 = 0xeab308;
inline constexpr uint32_t kHeatO1 = 0xf97316;
inline constexpr uint32_t kHeatR1 = 0xef4444;
inline constexpr uint32_t kHeatR2 = 0xb91c1c;
inline constexpr uint32_t kHeatNul = 0x374151;
inline constexpr uint32_t kAtmBg  = 0xf3f4f6;
inline constexpr uint32_t kAtmFg  = 0x000000;

inline ImU32 heatmap_color_for_iv(double iv_pct) noexcept {
    uint32_t hex;
    if      (iv_pct > 65.0) hex = kHeatR2;
    else if (iv_pct > 50.0) hex = kHeatR1;
    else if (iv_pct > 40.0) hex = kHeatO1;
    else if (iv_pct > 30.0) hex = kHeatY1;
    else if (iv_pct > 24.0) hex = kHeatG2;
    else if (iv_pct > 19.0) hex = kHeatG1;
    else if (iv_pct > 15.0) hex = kHeatB3;
    else if (iv_pct > 13.0) hex = kHeatB2;
    else                     hex = kHeatB1;
    return ImGui::ColorConvertFloat4ToU32(rgb(hex));
}

}  // namespace theme
