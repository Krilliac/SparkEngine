/**
 * @file TFUiCommon.h
 * @brief Shared ImGui helpers for the TERRAFRONT full-screen UIs
 *        (TFMapScreen / TFSpawnScreen): faction colors, centered text,
 *        pointy-top axial hex math.
 *
 * OWNERSHIP: ui agent (same as TFMapScreen/TFSpawnScreen). Header-only; all
 * ImGui-dependent content is compiled only under SPARK_HAS_IMGUI (the module's
 * ImGui rule) — safe to include from any UI translation unit.
 */
#pragma once

#include "Core/TFTypes.h"

#ifdef SPARK_HAS_IMGUI

#include <imgui.h>
#include <cmath>

namespace Terrafront::TFUi {

inline constexpr float kSqrt3   = 1.7320508f;
inline constexpr float kPi      = 3.14159265f;
inline constexpr float kDeg2Rad = kPi / 180.0f;

inline ImU32 FactionCol(FactionId f, float alpha = 1.0f)
{
    float c[4];
    FactionColor(f, c);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], alpha));
}

inline void AddTextCentered(ImDrawList* dl, float fontSize, ImVec2 center, ImU32 col,
                            const char* text)
{
    ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fontSize, 1e9f, 0.0f, text);
    dl->AddText(ImGui::GetFont(), fontSize,
                ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f), col, text);
}

/// Axial (q,r) -> pixel offset for pointy-top hexes, screen y grows downward.
/// regions.json uses r < 0 for north, which lands "up" on screen as intended.
inline ImVec2 AxialToPixel(int q, int r, float size)
{
    return ImVec2(size * kSqrt3 * (static_cast<float>(q) + static_cast<float>(r) * 0.5f),
                  size * 1.5f * static_cast<float>(r));
}

/// Pixel offset (relative to the axial origin) -> nearest axial hex (cube round).
inline void PixelToAxial(float x, float y, float size, int& outQ, int& outR)
{
    const float qf = (kSqrt3 / 3.0f * x - y / 3.0f) / size;
    const float rf = (2.0f / 3.0f * y) / size;
    float cx = qf, cz = rf, cy = -cx - cz;
    float rx = std::round(cx), ry = std::round(cy), rz = std::round(cz);
    const float dx = std::fabs(rx - cx), dy = std::fabs(ry - cy), dz = std::fabs(rz - cz);
    if (dx > dy && dx > dz)
        rx = -ry - rz;
    else if (dy > dz)
        ry = -rx - rz;
    else
        rz = -rx - ry;
    outQ = static_cast<int>(rx);
    outR = static_cast<int>(rz);
}

/// The 6 corners of a pointy-top hex centered at `c` (first corner straight up).
inline void HexCorners(ImVec2 c, float size, ImVec2 out[6])
{
    for (int i = 0; i < 6; ++i) {
        const float a = (-90.0f + 60.0f * static_cast<float>(i)) * kDeg2Rad;
        out[i] = ImVec2(c.x + size * std::cos(a), c.y + size * std::sin(a));
    }
}

/// Short display tag for a RegionDef tier string.
inline const char* TierTag(const std::string& tier)
{
    if (tier == "skyanchor") return "SKY";
    if (tier == "outpost")   return "OUT";
    if (tier == "fort")      return "FRT";
    if (tier == "facility")  return "FAC";
    return "?";
}

} // namespace Terrafront::TFUi

#endif // SPARK_HAS_IMGUI
