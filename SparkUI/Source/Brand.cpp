/**
 * @file Brand.cpp
 * @brief Logomark, brand gradient, soft shadow and brand header drawing.
 */

#include "SparkUI/Brand.h"

#include "SparkUI/Fonts.h"
#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>

namespace Spark::Ui
{
    namespace
    {
        constexpr float kPi = std::numbers::pi_v<float>;

        ImVec4 StopColour(int index, float alpha)
        {
            const std::uint32_t rgb = kBrandGradientStops[index];
            return {static_cast<float>((rgb >> 16u) & 0xFFu) / 255.0f, static_cast<float>((rgb >> 8u) & 0xFFu) / 255.0f,
                    static_cast<float>(rgb & 0xFFu) / 255.0f, alpha};
        }

        /// Three-stop gradient at t in [0, 1].
        ImU32 GradientAt(float t, float alpha)
        {
            t = std::clamp(t, 0.0f, 1.0f);
            const int segment = t < 0.5f ? 0 : 1;
            const float local = (t - 0.5f * static_cast<float>(segment)) * 2.0f;
            const ImVec4 a = StopColour(segment, alpha);
            const ImVec4 b = StopColour(segment + 1, alpha);
            return ImGui::ColorConvertFloat4ToU32(
                ImVec4(a.x + (b.x - a.x) * local, a.y + (b.y - a.y) * local, a.z + (b.z - a.z) * local, alpha));
        }

        /// Recolour vertices [first, end) by their position along the top-left to bottom-right diagonal.
        void ShadeDiagonal(ImDrawList* drawList, int first, ImVec2 min, ImVec2 max, float alpha)
        {
            const float dx = max.x - min.x;
            const float dy = max.y - min.y;
            const float lengthSq = dx * dx + dy * dy;
            for (int i = first; i < drawList->VtxBuffer.Size; ++i)
            {
                ImDrawVert& v = drawList->VtxBuffer[i];
                const float t = lengthSq > 0.0f ? ((v.pos.x - min.x) * dx + (v.pos.y - min.y) * dy) / lengthSq : 0.0f;
                const float vertexAlpha = static_cast<float>((v.col >> IM_COL32_A_SHIFT) & 0xFFu) / 255.0f;
                v.col = GradientAt(t, alpha * vertexAlpha);
            }
        }
    } // namespace

    void DrawLogomark(ImDrawList* drawList, ImVec2 center, float size, bool glow, ImU32 tint)
    {
        if (drawList == nullptr || size <= 0.0f)
        {
            return;
        }
        const ImVec2 origin(center.x - size * 0.5f, center.y - size * 0.5f);
        const auto at = [&](float ux, float uy) { return ImVec2(origin.x + ux * size, origin.y + uy * size); };
        const ImVec2 starCenter = at(Logomark::kCenterX, Logomark::kCenterY);

        if (glow)
        {
            const ImVec4 spark = StopColour(2, 1.0f);
            for (int ring = 4; ring >= 1; --ring)
            {
                const float radius = size * (0.30f + 0.06f * static_cast<float>(ring));
                drawList->AddCircleFilled(starCenter, radius,
                                          ImGui::ColorConvertFloat4ToU32(ImVec4(spark.x, spark.y, spark.z, 0.05f)));
            }
        }

        // Eight vertices alternating outer tip and inner notch; the first tip points up.
        std::array<ImVec2, 8> points{};
        const float rotation = Logomark::kRotationDegrees * kPi / 180.0f - kPi * 0.5f;
        for (int i = 0; i < 8; ++i)
        {
            const float radius = (i % 2 == 0 ? Logomark::kOuterRadius : Logomark::kInnerRadius) * size;
            const float angle = rotation + static_cast<float>(i) * kPi * 0.25f;
            points[static_cast<std::size_t>(i)] =
                ImVec2(starCenter.x + std::cos(angle) * radius, starCenter.y + std::sin(angle) * radius);
        }

        const int firstVertex = drawList->VtxBuffer.Size;
        const ImU32 fill = tint != 0 ? tint : IM_COL32_WHITE;
        for (std::size_t i = 0; i < points.size(); ++i)
        {
            drawList->AddTriangleFilled(starCenter, points[i], points[(i + 1) % points.size()], fill);
        }
        const ImVec2 dot = at(Logomark::kDotCenterX, Logomark::kDotCenterY);
        drawList->AddCircleFilled(dot, Logomark::kDotRadius * size, fill);
        if (tint == 0)
        {
            ShadeDiagonal(drawList, firstVertex, origin, ImVec2(origin.x + size, origin.y + size), 1.0f);
        }
    }

    void DrawBrandGradient(ImDrawList* drawList, ImVec2 min, ImVec2 max, float alpha, bool vertical)
    {
        if (drawList == nullptr || max.x <= min.x || max.y <= min.y)
        {
            return;
        }
        if (vertical)
        {
            const float middle = (min.y + max.y) * 0.5f;
            const ImU32 top = GradientAt(0.0f, alpha);
            const ImU32 mid = GradientAt(0.5f, alpha);
            const ImU32 bottom = GradientAt(1.0f, alpha);
            drawList->AddRectFilledMultiColor(min, ImVec2(max.x, middle), top, top, mid, mid);
            drawList->AddRectFilledMultiColor(ImVec2(min.x, middle), max, mid, mid, bottom, bottom);
            return;
        }
        drawList->AddRectFilledMultiColor(min, max, GradientAt(0.0f, alpha), GradientAt(0.5f, alpha),
                                          GradientAt(1.0f, alpha), GradientAt(0.5f, alpha));
    }

    void BrandHeader(const char* title, const char* subtitle)
    {
        const float markSize = Em(2.4f);
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        DrawLogomark(ImGui::GetWindowDrawList(), ImVec2(cursor.x + markSize * 0.5f, cursor.y + markSize * 0.5f),
                     markSize);
        ImGui::Dummy(ImVec2(markSize, markSize));
        ImGui::SameLine(0.0f, Px(12.0f));
        ImGui::BeginGroup();
        {
            ScopedFont display(FontRole::Display);
            ImGui::TextUnformatted(title != nullptr ? title : "");
        }
        if (subtitle != nullptr)
        {
            SecondaryText("%s", subtitle);
        }
        ImGui::EndGroup();
    }
} // namespace Spark::Ui
