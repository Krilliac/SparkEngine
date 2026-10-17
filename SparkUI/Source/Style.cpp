/**
 * @file Style.cpp
 * @brief Token set to ImGuiStyle mapping (absolute, idempotent).
 */

#include "SparkUI/Style.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace Spark::Ui
{
    namespace
    {
        void ApplyColours(const Tokens& t, ImGuiStyle& style)
        {
            ImVec4* c = style.Colors;
            const auto col = [&](Role r) { return ToImVec4(Get(t, r)); };
            const auto colA = [&](Role r, float alpha)
            {
                ImVec4 v = ToImVec4(Get(t, r));
                v.w *= alpha;
                return v;
            };

            c[ImGuiCol_Text] = col(Role::TextPrimary);
            c[ImGuiCol_TextDisabled] = col(Role::TextDisabled);
            c[ImGuiCol_WindowBg] = col(Role::SurfaceBase);
            c[ImGuiCol_ChildBg] = colA(Role::SurfaceBase, 0.0f);
            c[ImGuiCol_PopupBg] = col(Role::SurfaceOverlay);
            c[ImGuiCol_Border] = col(Role::BorderStrong);
            c[ImGuiCol_BorderShadow] = colA(Role::SurfaceSunken, 0.0f);
            c[ImGuiCol_FrameBg] = col(Role::SurfaceSunken);
            c[ImGuiCol_FrameBgHovered] = col(Role::SurfaceHover);
            c[ImGuiCol_FrameBgActive] = col(Role::SurfaceSelected);
            c[ImGuiCol_TitleBg] = col(Role::SurfaceSunken);
            c[ImGuiCol_TitleBgActive] = col(Role::SurfaceSunken);
            c[ImGuiCol_TitleBgCollapsed] = col(Role::SurfaceSunken);
            c[ImGuiCol_MenuBarBg] = col(Role::SurfaceSunken);
            c[ImGuiCol_ScrollbarBg] = col(Role::SurfaceSunken);
            c[ImGuiCol_ScrollbarGrab] = col(Role::BorderSubtle);
            c[ImGuiCol_ScrollbarGrabHovered] = col(Role::BorderStrong);
            c[ImGuiCol_ScrollbarGrabActive] = col(Role::AccentFill);
            c[ImGuiCol_CheckMark] = col(Role::AccentFill);
            c[ImGuiCol_CheckboxSelectedBg] = col(Role::SurfaceSelected);
            c[ImGuiCol_SliderGrab] = col(Role::AccentFill);
            c[ImGuiCol_SliderGrabActive] = col(Role::AccentFillHover);
            c[ImGuiCol_Button] = col(Role::SurfaceRaised);
            c[ImGuiCol_ButtonHovered] = col(Role::SurfaceHover);
            c[ImGuiCol_ButtonActive] = col(Role::SurfaceSelected);
            c[ImGuiCol_Header] = col(Role::SurfaceRaised);
            c[ImGuiCol_HeaderHovered] = col(Role::SurfaceHover);
            c[ImGuiCol_HeaderActive] = col(Role::SurfaceSelected);
            c[ImGuiCol_Separator] = col(Role::BorderSubtle);
            c[ImGuiCol_SeparatorHovered] = col(Role::BorderStrong);
            c[ImGuiCol_SeparatorActive] = col(Role::AccentFill);
            c[ImGuiCol_ResizeGrip] = colA(Role::BorderStrong, 0.25f);
            c[ImGuiCol_ResizeGripHovered] = col(Role::AccentFill);
            c[ImGuiCol_ResizeGripActive] = col(Role::AccentFillActive);
            c[ImGuiCol_InputTextCursor] = col(Role::AccentFill);
            c[ImGuiCol_TabHovered] = col(Role::SurfaceHover);
            c[ImGuiCol_Tab] = col(Role::SurfaceSunken);
            c[ImGuiCol_TabSelected] = col(Role::SurfaceBase);
            c[ImGuiCol_TabSelectedOverline] = col(Role::AccentFill);
            c[ImGuiCol_TabDimmed] = col(Role::SurfaceSunken);
            c[ImGuiCol_TabDimmedSelected] = col(Role::SurfaceBase);
            c[ImGuiCol_TabDimmedSelectedOverline] = col(Role::BorderStrong);
            c[ImGuiCol_DockingPreview] = col(Role::DropTarget);
            c[ImGuiCol_DockingEmptyBg] = col(Role::SurfaceSunken);
            c[ImGuiCol_PlotLines] = col(Role::Secondary);
            c[ImGuiCol_PlotLinesHovered] = col(Role::AccentFillHover);
            c[ImGuiCol_PlotHistogram] = col(Role::Graph1);
            c[ImGuiCol_PlotHistogramHovered] = col(Role::AccentFillHover);
            c[ImGuiCol_TableHeaderBg] = col(Role::SurfaceRaised);
            c[ImGuiCol_TableBorderStrong] = col(Role::BorderStrong);
            c[ImGuiCol_TableBorderLight] = col(Role::BorderSubtle);
            c[ImGuiCol_TableRowBg] = colA(Role::SurfaceBase, 0.0f);
            c[ImGuiCol_TableRowBgAlt] = colA(Role::SurfaceRaised, 0.45f);
            c[ImGuiCol_TextLink] = col(Role::TextLink);
            c[ImGuiCol_TextSelectedBg] = col(Role::SelectionTextBg);
            c[ImGuiCol_TreeLines] = col(Role::BorderSubtle);
            c[ImGuiCol_DragDropTarget] = col(Role::DropTarget);
            c[ImGuiCol_DragDropTargetBg] = colA(Role::Secondary, 0.12f);
            c[ImGuiCol_UnsavedMarker] = col(Role::AccentFill);
            c[ImGuiCol_NavCursor] = col(Role::BorderFocus);
            c[ImGuiCol_NavWindowingHighlight] = col(Role::BorderFocus);
            c[ImGuiCol_NavWindowingDimBg] = col(Role::ModalScrim);
            c[ImGuiCol_ModalWindowDimBg] = col(Role::ModalScrim);
        }
    } // namespace

    ImVec4 ToImVec4(Rgba c) noexcept
    {
        return {c.r, c.g, c.b, c.a};
    }

    float ClampScale(float dpiScale, float userScale) noexcept
    {
        const auto sane = [](float v) { return std::isfinite(v) && v > 0.0f ? v : 1.0f; };
        return std::clamp(sane(dpiScale) * sane(userScale), kMinUiScale, kMaxUiScale);
    }

    void ApplyStyle(const Tokens& t, float effectiveScale, ImGuiStyle& out)
    {
        const float s = ClampScale(effectiveScale, 1.0f);
        const auto px = [s](float logical) { return std::floor(logical * s); };
        const auto hairline = [s](float logical)
        { return logical <= 0.0f ? 0.0f : std::max(1.0f, std::round(logical * s)); };

        // Start from ImGui's defaults so the result never depends on a previously scaled style;
        // only the font scale factors (owned by the runtime) and the menu button side survive.
        ImGuiStyle style;
        style.ScaleAllSizes(s);

        const Metrics& m = t.metrics;
        style.WindowPadding = ImVec2(px(m.windowPadX), px(m.windowPadY));
        style.FramePadding = ImVec2(px(m.framePadX), px(m.framePadY));
        style.ItemSpacing = ImVec2(px(m.itemSpacingX), px(m.itemSpacingY));
        style.ItemInnerSpacing = ImVec2(px(m.itemInnerX), px(m.itemInnerY));
        style.CellPadding = ImVec2(px(m.cellPadX), px(m.cellPadY));
        style.SeparatorTextPadding = ImVec2(px(m.windowPadX * 2.5f), px(m.framePadY));
        style.IndentSpacing = px(m.indent);
        style.ScrollbarSize = px(m.scrollbar);
        style.GrabMinSize = px(m.grabMin);

        style.WindowRounding = px(m.rWindow);
        style.ChildRounding = px(m.rChild);
        style.FrameRounding = px(m.rFrame);
        style.PopupRounding = px(m.rPopup);
        style.ScrollbarRounding = px(m.rScrollbar);
        style.GrabRounding = px(m.rGrab);
        style.TabRounding = px(m.rTab);

        style.WindowBorderSize = hairline(m.bWindow);
        style.ChildBorderSize = hairline(m.bChild);
        style.PopupBorderSize = hairline(m.bPopup);
        style.FrameBorderSize = hairline(m.bFrame);
        style.TabBorderSize = 0.0f;
        style.TabBarBorderSize = hairline(1.0f);
        style.TabBarOverlineSize = hairline(m.tabOverline);
        style.SeparatorTextBorderSize = hairline(1.0f);
        style.DockingSeparatorSize = hairline(2.0f);

        style.WindowMenuButtonPosition = out.WindowMenuButtonPosition;
        style.FontSizeBase = t.type.body;
        style.FontScaleDpi = out.FontScaleDpi;
        style.FontScaleMain = out.FontScaleMain;

        ApplyColours(t, style);
        out = style;
    }
} // namespace Spark::Ui
