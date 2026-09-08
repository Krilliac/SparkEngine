/**
 * @file CSGEditorPanel.cpp
 * @brief ImGui rendering implementation for the CSG editor panel
 */

#include "CSGEditorPanel.h"

#include <cstdio>

#if __has_include(<imgui.h>)
#include <imgui.h>
#endif

namespace SparkEditor
{

    void CSGEditorPanel::Render()
    {
        // Without BeginPanel the widgets below land in ImGui's implicit fallback
        // window instead of a dockable, titled panel like every other editor panel.
        if (!BeginPanel())
        {
            EndPanel();
            return;
        }

        auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
        RenderBrushCreation(csg);
        RenderBrushList(csg);
        RenderBuildControls(csg);
        RenderStatistics();

        EndPanel();
    }

#if __has_include(<imgui.h>)
    void CSGEditorPanel::RenderBrushCreation(Spark::LevelDesign::CSGSystem& /*csg*/)
    {
        ImGui::TextUnformatted("Create Brush");
        ImGui::Separator();

        const char* shapes[] = {"Box", "Cylinder", "Sphere", "Wedge", "Cone"};
        int shapeIdx = static_cast<int>(m_brushShape);
        if (ImGui::Combo("Shape", &shapeIdx, shapes, IM_ARRAYSIZE(shapes)))
        {
            if (shapeIdx >= 0 && shapeIdx < IM_ARRAYSIZE(shapes))
                m_brushShape = static_cast<Spark::LevelDesign::BrushShape>(shapeIdx);
        }

        float size[3] = {m_brushSize.x, m_brushSize.y, m_brushSize.z};
        if (ImGui::DragFloat3("Size", size, 0.1f, 0.01f, 1000.0f))
        {
            m_brushSize = {size[0], size[1], size[2]};
        }

        const char* ops[] = {"Additive", "Subtractive", "Intersect"};
        int opIdx = static_cast<int>(m_brushOperation);
        if (ImGui::Combo("Operation", &opIdx, ops, IM_ARRAYSIZE(ops)))
        {
            if (opIdx >= 0 && opIdx < IM_ARRAYSIZE(ops))
                m_brushOperation = static_cast<Spark::LevelDesign::CSGOperation>(opIdx);
        }

        if (ImGui::Button("Create"))
        {
            CreateBrush(m_brushShape, m_brushSize, m_brushOperation);
        }
        ImGui::Spacing();
    }

    void CSGEditorPanel::RenderBrushList(Spark::LevelDesign::CSGSystem& csg)
    {
        ImGui::TextUnformatted("Brushes");
        ImGui::Separator();

        uint32_t toDelete = 0;
        for (uint32_t id : m_brushIds)
        {
            const auto* brush = csg.GetBrush(id);
            const bool selected = (id == m_selectedBrush);
            char label[64];
            std::snprintf(label, sizeof(label), "Brush %u%s", id, brush ? "" : " [missing]");
            if (ImGui::Selectable(label, selected))
            {
                m_selectedBrush = id;
            }
            ImGui::SameLine();
            ImGui::PushID(static_cast<int>(id));
            if (ImGui::SmallButton("X"))
            {
                toDelete = id;
            }
            ImGui::PopID();
        }
        if (toDelete != 0)
        {
            RemoveBrushById(toDelete);
        }
        ImGui::Spacing();
    }

    void CSGEditorPanel::RenderBuildControls(Spark::LevelDesign::CSGSystem& /*csg*/)
    {
        ImGui::TextUnformatted("Build");
        ImGui::Separator();
        ImGui::Checkbox("Auto-rebuild", &m_autoRebuild);
        ImGui::SameLine();
        if (ImGui::Button("Rebuild Now"))
        {
            RebuildMesh();
        }
        ImGui::Spacing();
    }

    void CSGEditorPanel::RenderStatistics()
    {
        ImGui::TextUnformatted("Statistics");
        ImGui::Separator();
        ImGui::Text("Brushes:   %u", static_cast<unsigned>(m_brushIds.size()));
        ImGui::Text("Triangles: %u", static_cast<unsigned>(m_lastMesh.triangleCount));
        ImGui::Text("Vertices:  %u", static_cast<unsigned>(m_lastMesh.vertices.size()));
        ImGui::Text("Selected:  %u", static_cast<unsigned>(m_selectedBrush));
    }
#else
    void CSGEditorPanel::RenderBrushCreation(Spark::LevelDesign::CSGSystem& /*csg*/) {}
    void CSGEditorPanel::RenderBrushList(Spark::LevelDesign::CSGSystem& /*csg*/) {}
    void CSGEditorPanel::RenderBuildControls(Spark::LevelDesign::CSGSystem& /*csg*/) {}
    void CSGEditorPanel::RenderStatistics() {}
#endif

} // namespace SparkEditor
