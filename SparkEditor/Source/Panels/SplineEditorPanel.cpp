/**
 * @file SplineEditorPanel.cpp
 * @brief Implementation of the spline path editor panel
 */

#include "SplineEditorPanel.h"
#include "../Core/EditorIcons.h"
#include <imgui.h>
#include <iostream>
#include <cstdio>
#include <cmath>
#include "Utils/LogMacros.h"

namespace SparkEditor
{

    SplineEditorPanel::SplineEditorPanel() : EditorPanel("Spline Editor", "spline_editor_panel") {}

    bool SplineEditorPanel::Initialize()
    {
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Initializing Spline Editor panel");
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
            RenderSplineMetrics();
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
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Shutting down Spline Editor panel");
    }

    void SplineEditorPanel::RenderSplineSettings()
    {
        const char* splineTypes[] = {"Catmull-Rom", "Bezier", "Hermite", "Linear"};
        ImGui::Combo("Spline Type", &m_splineType, splineTypes, IM_ARRAYSIZE(splineTypes));
        ImGui::SameLine();
        ImGui::Checkbox("Closed Loop", &m_closedLoop);

        if (m_splineType == 0) // Catmull-Rom
            ImGui::DragFloat("Tension", &m_tension, 0.01f, 0.0f, 1.0f);

        ImGui::Checkbox("Show Tangents", &m_showTangents);

        // Path following controls
        ImGui::SameLine();
        ImGui::Checkbox("Constant Speed", &m_constantSpeed);

        ImGui::DragFloat("Path Speed", &m_pathSpeed, 0.1f, 0.01f, 100.0f, "%.1f m/s");
        ImGui::SameLine();
        ImGui::DragFloat("Duration", &m_pathDuration, 0.1f, 0.1f, 600.0f, "%.1f s");
    }

    void SplineEditorPanel::RenderSplineMetrics()
    {
        int pointCount = static_cast<int>(m_points.size());
        int segmentCount = m_closedLoop ? pointCount : (pointCount > 1 ? pointCount - 1 : 0);
        float approxLength = CalculateApproxLength();

        ImGui::Text(ICON_FA_BEZIER_CURVE " Points: %d | Segments: %d | Length: %.1f m", pointCount, segmentCount,
                    approxLength);
    }

    void SplineEditorPanel::RenderPointList()
    {
        if (ImGui::Button(ICON_FA_PLUS " Add"))
        {
            ControlPoint pt;
            if (!m_points.empty())
            {
                auto& last = m_points.back();
                pt.position = {last.position.x + 1.0f, last.position.y, last.position.z};
            }
            m_points.push_back(pt);
            m_selectedPoint = static_cast<int>(m_points.size()) - 1;
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_EXCHANGE " Reverse") && m_points.size() > 1)
        {
            std::reverse(m_points.begin(), m_points.end());
            if (m_selectedPoint >= 0)
                m_selectedPoint = static_cast<int>(m_points.size()) - 1 - m_selectedPoint;
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

        // Tangent editing for Hermite/Bezier
        if (m_splineType == 1 || m_splineType == 2)
        {
            float tan[3] = {pt.tangent.x, pt.tangent.y, pt.tangent.z};
            if (ImGui::DragFloat3("Tangent", tan, 0.1f))
                pt.tangent = {tan[0], tan[1], tan[2]};
        }

        ImGui::Separator();

        // Insert point before/after
        if (ImGui::Button(ICON_FA_PLUS " Insert Before"))
        {
            ControlPoint newPt;
            if (m_selectedPoint > 0)
            {
                auto& prev = m_points[m_selectedPoint - 1];
                newPt.position = {(prev.position.x + pt.position.x) * 0.5f, (prev.position.y + pt.position.y) * 0.5f,
                                  (prev.position.z + pt.position.z) * 0.5f};
            }
            else
            {
                newPt.position = {pt.position.x - 1.0f, pt.position.y, pt.position.z};
            }
            m_points.insert(m_points.begin() + m_selectedPoint, newPt);
            ++m_selectedPoint;
        }

        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS " Insert After"))
        {
            ControlPoint newPt;
            if (m_selectedPoint < static_cast<int>(m_points.size()) - 1)
            {
                auto& next = m_points[m_selectedPoint + 1];
                newPt.position = {(pt.position.x + next.position.x) * 0.5f, (pt.position.y + next.position.y) * 0.5f,
                                  (pt.position.z + next.position.z) * 0.5f};
            }
            else
            {
                newPt.position = {pt.position.x + 1.0f, pt.position.y, pt.position.z};
            }
            m_points.insert(m_points.begin() + m_selectedPoint + 1, newPt);
        }

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

    float SplineEditorPanel::CalculateApproxLength() const
    {
        if (m_points.size() < 2)
            return 0.0f;

        float length = 0.0f;
        for (int i = 1; i < static_cast<int>(m_points.size()); ++i)
        {
            float dx = m_points[i].position.x - m_points[i - 1].position.x;
            float dy = m_points[i].position.y - m_points[i - 1].position.y;
            float dz = m_points[i].position.z - m_points[i - 1].position.z;
            length += std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        if (m_closedLoop && m_points.size() >= 2)
        {
            auto& first = m_points.front();
            auto& last = m_points.back();
            float dx = first.position.x - last.position.x;
            float dy = first.position.y - last.position.y;
            float dz = first.position.z - last.position.z;
            length += std::sqrt(dx * dx + dy * dy + dz * dz);
        }

        return length;
    }

} // namespace SparkEditor
