/**
 * @file SpriteEditorPanel.cpp
 * @brief Implementation of the Sprite Editor panel
 * @author Spark Engine Team
 * @date 2026
 */

#include "SpriteEditorPanel.h"
#include "../Core/EditorIcons.h"
#include "../Core/EditorFonts.h"
#include "../../../SparkEngine/Source/Utils/Validate.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>
#include <cstring>
#include <array>
#include <cmath>
#include "Utils/LogMacros.h"

#include "SparkUI/Runtime.h"
#include "SparkUI/Widgets.h"

namespace SparkEditor
{

    namespace
    {
        /// @brief One quick tint swatch: these are sprite data values, not UI chrome colours.
        struct QuickTint
        {
            const char* name;
            float rgba[4];
        };

        constexpr std::array<QuickTint, 5> kQuickTints = {{
            {"White", {1.0f, 1.0f, 1.0f, 1.0f}},
            {"Red", {1.0f, 0.0f, 0.0f, 1.0f}},
            {"Green", {0.0f, 1.0f, 0.0f, 1.0f}},
            {"Blue", {0.0f, 0.0f, 1.0f, 1.0f}},
            {"Half Alpha", {1.0f, 1.0f, 1.0f, 0.5f}},
        }};
    } // namespace

    SpriteEditorPanel::SpriteEditorPanel() : EditorPanel("Sprite Editor", "sprite_editor_panel") {}

    bool SpriteEditorPanel::Initialize()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Sprite Editor panel");
        return true;
    }

    void SpriteEditorPanel::Update(float /*deltaTime*/)
    {
        // Nothing frame-rate dependent
    }

    void SpriteEditorPanel::Render()
    {
        if (!IsVisible())
        {
            return;
        }

        if (BeginPanel())
        {
            if (m_inspectedObjectID == INVALID_OBJECT_ID)
            {
                float avail = ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.3f);
                Spark::Ui::EmptyState(ICON_FA_IMAGE, "Select a sprite entity to edit");
            }
            else
            {
                // Toolbar
                if (Spark::Ui::IconButton("ZoomIn", ICON_FA_SEARCH_PLUS, "Zoom in"))
                {
                    m_previewZoom *= 1.25f;
                }
                ImGui::SameLine();
                if (Spark::Ui::IconButton("ZoomOut", ICON_FA_SEARCH_MINUS, "Zoom out"))
                {
                    m_previewZoom *= 0.8f;
                }
                ImGui::SameLine();
                if (Spark::Ui::IconButton("ResetView", ICON_FA_CROSSHAIRS, "Reset zoom and pan"))
                {
                    m_previewZoom = 1.0f;
                    m_previewPanX = 0.0f;
                    m_previewPanY = 0.0f;
                }
                ImGui::SameLine();
                ImGui::Checkbox("Grid", &m_showGrid);
                ImGui::SameLine();
                ImGui::Checkbox("Pivot", &m_showPivot);

                ImGui::Separator();

                // Split: preview on left, properties on right
                float panelWidth = ImGui::GetContentRegionAvail().x;
                float previewWidth = panelWidth * 0.55f;

                ImGui::BeginChild("SpritePreview", ImVec2(previewWidth, 0), true);
                RenderTexturePreview();
                ImGui::EndChild();

                ImGui::SameLine();

                ImGui::BeginChild("SpriteProperties", ImVec2(0, 0), true);
                RenderSpriteProperties();
                ImGui::EndChild();
            }
        }
        // ImGui::Begin was called even when BeginPanel() reports a collapsed or hidden-tab window.
        EndPanel();
    }

    void SpriteEditorPanel::Shutdown()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Sprite Editor panel");
    }

    void SpriteEditorPanel::RenderTexturePreview()
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float previewSize = (std::min)(avail.x, avail.y) * 0.95f;

        // Draw checkerboard background for transparency
        ImVec2 canvasPos = ImGui::GetCursorScreenPos();
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Transparency checkerboard in two neutral surface tones.
        const ImU32 checkerLight = Spark::Ui::Col(Spark::Ui::Role::SurfaceHover);
        const ImU32 checkerDark = Spark::Ui::Col(Spark::Ui::Role::SurfaceRaised);
        float gridStep = Spark::Ui::Px(8.0f) * m_previewZoom;
        for (float y = 0; y < previewSize; y += gridStep)
        {
            for (float x = 0; x < previewSize; x += gridStep)
            {
                int ix = static_cast<int>(x / gridStep);
                int iy = static_cast<int>(y / gridStep);
                ImU32 col = ((ix + iy) % 2 == 0) ? checkerLight : checkerDark;
                drawList->AddRectFilled(ImVec2(canvasPos.x + x, canvasPos.y + y),
                                        ImVec2(canvasPos.x + (std::min)(x + gridStep, previewSize),
                                               canvasPos.y + (std::min)(y + gridStep, previewSize)),
                                        col);
            }
        }

        // Draw sprite bounds area (no GPU texture — render outline and info)
        float spriteW = previewSize * 0.8f * m_previewZoom;
        float spriteH = previewSize * 0.8f * m_previewZoom;
        float ox = canvasPos.x + (previewSize - spriteW) * 0.5f + m_previewPanX;
        float oy = canvasPos.y + (previewSize - spriteH) * 0.5f + m_previewPanY;

        // Sprite region fill with subtle gradient effect using two half-rects
        float halfH = spriteH * 0.5f;
        drawList->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + spriteW, oy + halfH),
                                Spark::Ui::Col(Spark::Ui::Role::AccentFillHover, 0.55f));
        drawList->AddRectFilled(ImVec2(ox, oy + halfH), ImVec2(ox + spriteW, oy + spriteH),
                                Spark::Ui::Col(Spark::Ui::Role::AccentFillActive, 0.55f));

        // Outer border
        drawList->AddRect(ImVec2(ox, oy), ImVec2(ox + spriteW, oy + spriteH),
                          Spark::Ui::Col(Spark::Ui::Role::TextPrimary, 0.86f), 0.0f, 0, Spark::Ui::Px(2.0f));

        // Diagonal cross to indicate missing texture
        const ImU32 crossCol = Spark::Ui::Col(Spark::Ui::Role::TextPrimary, 0.24f);
        drawList->AddLine(ImVec2(ox, oy), ImVec2(ox + spriteW, oy + spriteH), crossCol);
        drawList->AddLine(ImVec2(ox + spriteW, oy), ImVec2(ox, oy + spriteH), crossCol);

        // Sprite dimensions label
        char dimLabel[64];
        snprintf(dimLabel, sizeof(dimLabel), "%.0f x %.0f", spriteW, spriteH);
        ImVec2 dimSize = ImGui::CalcTextSize(dimLabel);
        drawList->AddText(ImVec2(ox + (spriteW - dimSize.x) * 0.5f, oy + spriteH + Spark::Ui::Px(4.0f)),
                          Spark::Ui::Col(Spark::Ui::Role::TextSecondary), dimLabel);

        // Draw source rect highlight as dashed-style inner border
        if (m_showGrid)
        {
            const float inset = Spark::Ui::Px(4.0f);
            ImVec2 srcMin(ox + inset, oy + inset);
            ImVec2 srcMax(ox + spriteW - inset, oy + spriteH - inset);
            drawList->AddRect(srcMin, srcMax, Spark::Ui::Col(Spark::Ui::Role::SuccessText, 0.7f), 0.0f, 0,
                              Spark::Ui::Px(1.5f));

            // Corner markers for source rect
            const float cornerLen = Spark::Ui::Px(8.0f);
            const float cornerThickness = Spark::Ui::Px(2.0f);
            const ImU32 cornerCol = Spark::Ui::Col(Spark::Ui::Role::SuccessText);
            // Top-left
            drawList->AddLine(srcMin, ImVec2(srcMin.x + cornerLen, srcMin.y), cornerCol, cornerThickness);
            drawList->AddLine(srcMin, ImVec2(srcMin.x, srcMin.y + cornerLen), cornerCol, cornerThickness);
            // Top-right
            drawList->AddLine(ImVec2(srcMax.x, srcMin.y), ImVec2(srcMax.x - cornerLen, srcMin.y), cornerCol,
                              cornerThickness);
            drawList->AddLine(ImVec2(srcMax.x, srcMin.y), ImVec2(srcMax.x, srcMin.y + cornerLen), cornerCol,
                              cornerThickness);
            // Bottom-left
            drawList->AddLine(ImVec2(srcMin.x, srcMax.y), ImVec2(srcMin.x + cornerLen, srcMax.y), cornerCol,
                              cornerThickness);
            drawList->AddLine(ImVec2(srcMin.x, srcMax.y), ImVec2(srcMin.x, srcMax.y - cornerLen), cornerCol,
                              cornerThickness);
            // Bottom-right
            drawList->AddLine(srcMax, ImVec2(srcMax.x - cornerLen, srcMax.y), cornerCol, cornerThickness);
            drawList->AddLine(srcMax, ImVec2(srcMax.x, srcMax.y - cornerLen), cornerCol, cornerThickness);
        }

        // Draw pivot point
        if (m_showPivot)
        {
            float px = ox + spriteW * 0.5f;
            float py = oy + spriteH * 0.5f;
            const float armLength = Spark::Ui::Px(10.0f);
            const ImU32 armCol = Spark::Ui::Col(Spark::Ui::Role::TextPrimary, .78f);
            drawList->AddCircleFilled(ImVec2(px, py), Spark::Ui::Px(5.0f), Spark::Ui::Col(Spark::Ui::Role::ErrorText));
            drawList->AddLine(ImVec2(px - armLength, py), ImVec2(px + armLength, py), armCol, Spark::Ui::Px(1.0f));
            drawList->AddLine(ImVec2(px, py - armLength), ImVec2(px, py + armLength), armCol, Spark::Ui::Px(1.0f));
        }

        ImGui::Dummy(ImVec2(previewSize, previewSize));

        // Handle pan with middle mouse
        if (ImGui::IsItemHovered())
        {
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
            {
                ImVec2 delta = ImGui::GetIO().MouseDelta;
                m_previewPanX += delta.x;
                m_previewPanY += delta.y;
            }
            float wheel = ImGui::GetIO().MouseWheel;
            if (std::abs(wheel) > 0.01f)
            {
                m_previewZoom *= (wheel > 0) ? 1.1f : 0.9f;
                m_previewZoom = std::clamp(m_previewZoom, 0.1f, 10.0f);
            }
        }
    }

    void SpriteEditorPanel::RenderSpriteProperties()
    {
        Spark::Ui::SubHeader(ICON_FA_IMAGE " Sprite Properties");

        RenderSourceRectEditor();
        ImGui::Spacing();

        RenderPivotEditor();
        ImGui::Spacing();

        RenderSortingSettings();
        ImGui::Spacing();

        RenderColorPicker();
    }

    void SpriteEditorPanel::RenderSourceRectEditor()
    {
        if (Spark::Ui::SectionHeader("Source Rectangle", ICON_FA_CROP))
        {
            static float sourceRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            ImGui::DragFloat2("UV Min", &sourceRect[0], 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat2("UV Max", &sourceRect[2], 0.01f, 0.0f, 1.0f);

            ImGui::Spacing();
            Spark::Ui::SecondaryText("Quick presets:");
            if (ImGui::Button("Full Texture"))
            {
                sourceRect[0] = 0.0f;
                sourceRect[1] = 0.0f;
                sourceRect[2] = 1.0f;
                sourceRect[3] = 1.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Top-Left Quarter"))
            {
                sourceRect[0] = 0.0f;
                sourceRect[1] = 0.0f;
                sourceRect[2] = 0.5f;
                sourceRect[3] = 0.5f;
            }

            // Sprite sheet auto-slice
            static int sliceCols = 4;
            static int sliceRows = 4;
            static int selectedCell = 0;
            Spark::Ui::SubHeader("Sprite Sheet Slicer");
            ImGui::DragInt("Columns", &sliceCols, 0.1f, 1, 64);
            ImGui::DragInt("Rows", &sliceRows, 0.1f, 1, 64);
            ImGui::DragInt("Cell Index", &selectedCell, 0.1f, 0, sliceCols * sliceRows - 1);

            if (ImGui::Button("Apply Cell"))
            {
                int col = selectedCell % sliceCols;
                int row = selectedCell / sliceCols;
                sourceRect[0] = static_cast<float>(col) / sliceCols;
                sourceRect[1] = static_cast<float>(row) / sliceRows;
                sourceRect[2] = static_cast<float>(col + 1) / sliceCols;
                sourceRect[3] = static_cast<float>(row + 1) / sliceRows;
            }
        }
    }

    void SpriteEditorPanel::RenderPivotEditor()
    {
        if (Spark::Ui::SectionHeader("Pivot Point", ICON_FA_CROSSHAIRS))
        {
            static float pivot[2] = {0.5f, 0.5f};
            ImGui::DragFloat2("Pivot", pivot, 0.01f, 0.0f, 1.0f);

            Spark::Ui::SecondaryText("Presets:");
            if (ImGui::Button("Center"))
            {
                pivot[0] = 0.5f;
                pivot[1] = 0.5f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Bottom"))
            {
                pivot[0] = 0.5f;
                pivot[1] = 1.0f;
            }
            ImGui::SameLine();
            if (ImGui::Button("Top-Left"))
            {
                pivot[0] = 0.0f;
                pivot[1] = 0.0f;
            }

            static float pixelsPerUnit = 100.0f;
            ImGui::DragFloat("Pixels Per Unit", &pixelsPerUnit, 1.0f, 1.0f, 1000.0f);
        }
    }

    void SpriteEditorPanel::RenderSortingSettings()
    {
        if (Spark::Ui::SectionHeader("Sorting", ICON_FA_LAYER_GROUP))
        {
            static int sortingLayer = 0;
            static int orderInLayer = 0;
            ImGui::DragInt("Sorting Layer", &sortingLayer);
            ImGui::DragInt("Order in Layer", &orderInLayer);

            static bool flipX = false;
            static bool flipY = false;
            ImGui::Checkbox("Flip X", &flipX);
            ImGui::SameLine();
            ImGui::Checkbox("Flip Y", &flipY);
        }
    }

    void SpriteEditorPanel::RenderColorPicker()
    {
        if (Spark::Ui::SectionHeader("Color & Tint", ICON_FA_PALETTE))
        {
            static float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            ImGui::ColorEdit4("Tint Color", color, ImGuiColorEditFlags_AlphaBar);

            Spark::Ui::SecondaryText("Quick Colors:");
            bool firstSwatch = true;
            for (const QuickTint& tint : kQuickTints)
            {
                if (!firstSwatch)
                {
                    ImGui::SameLine();
                }
                firstSwatch = false;
                if (ImGui::ColorButton(tint.name, ImVec4(tint.rgba[0], tint.rgba[1], tint.rgba[2], tint.rgba[3])))
                {
                    std::copy(std::begin(tint.rgba), std::end(tint.rgba), color);
                }
            }
        }
    }

} // namespace SparkEditor
