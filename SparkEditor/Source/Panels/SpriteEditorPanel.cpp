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
#include <cmath>
#include "Utils/LogMacros.h"

namespace SparkEditor
{

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
            return;

        if (BeginPanel())
        {
            if (m_inspectedObjectID == INVALID_OBJECT_ID)
            {
                float avail = ImGui::GetContentRegionAvail().y;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail * 0.4f);
                float textWidth = ImGui::CalcTextSize(ICON_FA_IMAGE " Select a sprite entity to edit").x;
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - textWidth) * 0.5f);
                ImGui::TextColored(ImVec4(0.55f, 0.58f, 0.62f, 1.0f), ICON_FA_IMAGE " Select a sprite entity to edit");
            }
            else
            {
                // Toolbar
                if (ImGui::Button(ICON_FA_SEARCH_PLUS))
                    m_previewZoom *= 1.25f;
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_SEARCH_MINUS))
                    m_previewZoom *= 0.8f;
                ImGui::SameLine();
                if (ImGui::Button(ICON_FA_CROSSHAIRS))
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
            EndPanel();
        }
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

        float gridStep = 8.0f * m_previewZoom;
        for (float y = 0; y < previewSize; y += gridStep)
        {
            for (float x = 0; x < previewSize; x += gridStep)
            {
                int ix = static_cast<int>(x / gridStep);
                int iy = static_cast<int>(y / gridStep);
                ImU32 col = ((ix + iy) % 2 == 0) ? IM_COL32(200, 200, 200, 255) : IM_COL32(150, 150, 150, 255);
                drawList->AddRectFilled(ImVec2(canvasPos.x + x, canvasPos.y + y),
                                        ImVec2(canvasPos.x + (std::min)(x + gridStep, previewSize),
                                               canvasPos.y + (std::min)(y + gridStep, previewSize)),
                                        col);
            }
        }

        // Draw sprite preview rectangle (placeholder without actual GPU texture)
        float spriteW = previewSize * 0.8f * m_previewZoom;
        float spriteH = previewSize * 0.8f * m_previewZoom;
        float ox = canvasPos.x + (previewSize - spriteW) * 0.5f + m_previewPanX;
        float oy = canvasPos.y + (previewSize - spriteH) * 0.5f + m_previewPanY;

        drawList->AddRectFilled(ImVec2(ox, oy), ImVec2(ox + spriteW, oy + spriteH), IM_COL32(100, 150, 255, 180));
        drawList->AddRect(ImVec2(ox, oy), ImVec2(ox + spriteW, oy + spriteH), IM_COL32(255, 255, 255, 200), 0.0f, 0,
                          1.0f);

        // Draw source rect highlight
        if (m_showGrid)
        {
            drawList->AddRect(ImVec2(ox + 2, oy + 2), ImVec2(ox + spriteW - 2, oy + spriteH - 2),
                              IM_COL32(0, 255, 0, 150), 0.0f, 0, 2.0f);
        }

        // Draw pivot point
        if (m_showPivot)
        {
            float px = ox + spriteW * 0.5f;
            float py = oy + spriteH * 0.5f;
            drawList->AddCircleFilled(ImVec2(px, py), 5.0f, IM_COL32(255, 50, 50, 255));
            drawList->AddLine(ImVec2(px - 10, py), ImVec2(px + 10, py), IM_COL32(255, 255, 255, 200), 1.0f);
            drawList->AddLine(ImVec2(px, py - 10), ImVec2(px, py + 10), IM_COL32(255, 255, 255, 200), 1.0f);
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
        ImGui::Text(ICON_FA_IMAGE " Sprite Properties");
        ImGui::Separator();

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
        if (ImGui::CollapsingHeader(ICON_FA_CROP " Source Rectangle", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static float sourceRect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            ImGui::DragFloat2("UV Min", &sourceRect[0], 0.01f, 0.0f, 1.0f);
            ImGui::DragFloat2("UV Max", &sourceRect[2], 0.01f, 0.0f, 1.0f);

            ImGui::Spacing();
            ImGui::Text("Quick presets:");
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
            ImGui::Separator();
            static int sliceCols = 4;
            static int sliceRows = 4;
            static int selectedCell = 0;
            ImGui::Text("Sprite Sheet Slicer:");
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
        if (ImGui::CollapsingHeader(ICON_FA_CROSSHAIRS " Pivot Point", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static float pivot[2] = {0.5f, 0.5f};
            ImGui::DragFloat2("Pivot", pivot, 0.01f, 0.0f, 1.0f);

            ImGui::Text("Presets:");
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
        if (ImGui::CollapsingHeader(ICON_FA_LAYER_GROUP " Sorting", ImGuiTreeNodeFlags_DefaultOpen))
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
        if (ImGui::CollapsingHeader(ICON_FA_PALETTE " Color & Tint", ImGuiTreeNodeFlags_DefaultOpen))
        {
            static float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            ImGui::ColorEdit4("Tint Color", color, ImGuiColorEditFlags_AlphaBar);

            ImGui::Text("Quick Colors:");
            if (ImGui::ColorButton("White", ImVec4(1, 1, 1, 1)))
            {
                color[0] = 1;
                color[1] = 1;
                color[2] = 1;
                color[3] = 1;
            }
            ImGui::SameLine();
            if (ImGui::ColorButton("Red", ImVec4(1, 0, 0, 1)))
            {
                color[0] = 1;
                color[1] = 0;
                color[2] = 0;
                color[3] = 1;
            }
            ImGui::SameLine();
            if (ImGui::ColorButton("Green", ImVec4(0, 1, 0, 1)))
            {
                color[0] = 0;
                color[1] = 1;
                color[2] = 0;
                color[3] = 1;
            }
            ImGui::SameLine();
            if (ImGui::ColorButton("Blue", ImVec4(0, 0, 1, 1)))
            {
                color[0] = 0;
                color[1] = 0;
                color[2] = 1;
                color[3] = 1;
            }
            ImGui::SameLine();
            if (ImGui::ColorButton("Half Alpha", ImVec4(1, 1, 1, 0.5f)))
            {
                color[0] = 1;
                color[1] = 1;
                color[2] = 1;
                color[3] = 0.5f;
            }
        }
    }

} // namespace SparkEditor
