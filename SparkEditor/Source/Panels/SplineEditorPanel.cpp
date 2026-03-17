/**
 * @file SplineEditorPanel.cpp
 * @brief Implementation of the spline path editor panel
 */

#include "SplineEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>

namespace SparkEditor
{

    SplineEditorPanel::SplineEditorPanel() : EditorPanel("Spline Editor", "spline_editor_panel") {}

    bool SplineEditorPanel::Initialize()
    {
        std::cout << "Initializing Spline Editor panel\n";
        return true;
    }

    void SplineEditorPanel::Update(float /*deltaTime*/) {}

    void SplineEditorPanel::Render()
    {
        if (!IsVisible())
            return;

        if (BeginPanel())
        {
            RenderSplineSettings();
            ImGui::Separator();

            float listWidth = ImGui::GetContentRegionAvail().x * 0.35f;
            ImGui::BeginChild("PointList", ImVec2(listWidth, 0), true);
            RenderPointList();
            ImGui::EndChild();

            ImGui::SameLine();

            ImGui::BeginChild("PointEditor", ImVec2(0, 0), true);
            RenderPointEditor();
            ImGui::EndChild();
        }
        EndPanel();
    }

    void SplineEditorPanel::Shutdown()
    {
        std::cout << "Shutting down Spline Editor panel\n";
    }

    void SplineEditorPanel::RenderSplineSettings()
    {
        const char* splineTypes[] = {"Catmull-Rom", "Bezier"};
        ImGui::Combo("Spline Type", &m_splineType, splineTypes, IM_ARRAYSIZE(splineTypes));
        ImGui::SameLine();
        ImGui::Checkbox("Closed Loop", &m_closedLoop);

        if (m_splineType == 0) // Catmull-Rom
            ImGui::DragFloat("Tension", &m_tension, 0.01f, 0.0f, 1.0f);

        ImGui::Checkbox("Show Tangents", &m_showTangents);
    }

    void SplineEditorPanel::RenderPointList()
    {
        if (ImGui::Button(ICON_FA_PLUS " Add Point"))
        {
            ControlPoint pt;
            if (!m_points.empty())
            {
                // Place new point offset from last
                auto& last = m_points.back();
                pt.position = {last.position.x + 1.0f, last.position.y, last.position.z};
            }
            m_points.push_back(pt);
            m_selectedPoint = static_cast<int>(m_points.size()) - 1;
        }

        ImGui::Separator();

        for (int i = 0; i < static_cast<int>(m_points.size()); ++i)
        {
            char label[64];
            snprintf(label, sizeof(label), "Point %d (%.1f, %.1f, %.1f)", i, m_points[i].position.x,
                     m_points[i].position.y, m_points[i].position.z);

            if (ImGui::Selectable(label, m_selectedPoint == i))
                m_selectedPoint = i;
        }
    }

    void SplineEditorPanel::RenderPointEditor()
    {
        if (m_selectedPoint < 0 || m_selectedPoint >= static_cast<int>(m_points.size()))
        {
            ImGui::TextDisabled("Select a control point to edit");
            return;
        }

        auto& pt = m_points[m_selectedPoint];
        ImGui::Text("Point %d", m_selectedPoint);

        float pos[3] = {pt.position.x, pt.position.y, pt.position.z};
        if (ImGui::DragFloat3("Position", pos, 0.1f))
            pt.position = {pos[0], pos[1], pos[2]};

        ImGui::Separator();

        if (ImGui::Button(ICON_FA_TRASH " Delete Point") && m_points.size() > 1)
        {
            m_points.erase(m_points.begin() + m_selectedPoint);
            if (m_selectedPoint >= static_cast<int>(m_points.size()))
                m_selectedPoint = static_cast<int>(m_points.size()) - 1;
        }

        if (m_selectedPoint > 0)
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_UP " Move Up"))
            {
                std::swap(m_points[m_selectedPoint], m_points[m_selectedPoint - 1]);
                --m_selectedPoint;
            }
        }

        if (m_selectedPoint < static_cast<int>(m_points.size()) - 1)
        {
            ImGui::SameLine();
            if (ImGui::Button(ICON_FA_ARROW_DOWN " Move Down"))
            {
                std::swap(m_points[m_selectedPoint], m_points[m_selectedPoint + 1]);
                ++m_selectedPoint;
            }
        }
    }

} // namespace SparkEditor
