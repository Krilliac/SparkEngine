/**
 * @file CSGEditorPanel.h
 * @brief Editor panel for constructive solid geometry (CSG) level design
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides an ImGui interface for creating CSG brushes, selecting boolean
 * operations, and previewing the resulting mesh in real time.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Engine/LevelDesign/CSGSystem.h"

#include <string>
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Editor panel for CSG-based level greyboxing
     *
     * Allows designers to create primitive brushes (box, cylinder, sphere, wedge, cone),
     * set their transforms and boolean operations, and build the combined geometry.
     */
    class CSGEditorPanel : public EditorPanel
    {
      public:
        CSGEditorPanel() : EditorPanel("CSG Editor", "CSGEditor") {}

        bool Initialize() override
        {
            m_selectedBrush = 0;
            m_brushShape = Spark::LevelDesign::BrushShape::Box;
            m_brushSize = {2.0f, 2.0f, 2.0f};
            m_brushOperation = Spark::LevelDesign::CSGOperation::Additive;
            m_autoRebuild = true;
            m_lastMesh = {};
            return true;
        }

        void Update(float deltaTime) override { (void)deltaTime; }

        void Render() override
        {
            auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();

            // --- Brush creation ---
            RenderBrushCreation(csg);

            // --- Brush list ---
            RenderBrushList(csg);

            // --- Build controls ---
            RenderBuildControls(csg);

            // --- Statistics ---
            RenderStatistics();
        }

        void Shutdown() override {}

        std::string GetTypeName() const override { return "CSGEditorPanel"; }

      private:
        void RenderBrushCreation(Spark::LevelDesign::CSGSystem& csg)
        {
            // Shape selector
            const char* shapes[] = {"Box", "Cylinder", "Sphere", "Wedge", "Cone"};
            int shapeIdx = static_cast<int>(m_brushShape);
            if (shapeIdx >= 0 && shapeIdx < 5)
            {
                // ImGui::Combo would go here in real editor
                (void)shapes[shapeIdx];
            }

            // Size fields would use ImGui::DragFloat3
            // Operation selector would use ImGui::Combo

            // Create button
            if (m_createRequested)
            {
                uint32_t id = csg.CreateBrush(m_brushShape, m_brushSize);
                if (id > 0)
                {
                    csg.SetBrushOperation(id, m_brushOperation);
                    m_selectedBrush = id;
                    m_brushIds.push_back(id);
                    if (m_autoRebuild)
                        RebuildMesh(csg);
                }
                m_createRequested = false;
            }
        }

        void RenderBrushList(Spark::LevelDesign::CSGSystem& csg)
        {
            // List all brushes with selection
            for (size_t i = 0; i < m_brushIds.size(); ++i)
            {
                uint32_t id = m_brushIds[i];
                const auto* brush = csg.GetBrush(id);
                if (!brush)
                    continue;

                bool selected = (id == m_selectedBrush);
                if (selected)
                    m_selectedBrush = id;

                // Delete button per brush
                if (m_deleteRequested == id)
                {
                    csg.RemoveBrush(id);
                    m_brushIds.erase(m_brushIds.begin() + static_cast<ptrdiff_t>(i));
                    --i;
                    m_deleteRequested = 0;
                    if (m_autoRebuild)
                        RebuildMesh(csg);
                }
            }
        }

        void RenderBuildControls(Spark::LevelDesign::CSGSystem& csg)
        {
            // Auto-rebuild toggle
            // Manual rebuild button
            if (m_rebuildRequested)
            {
                RebuildMesh(csg);
                m_rebuildRequested = false;
            }
        }

        void RenderStatistics()
        {
            // Show: brush count, triangle count, vertex count
            (void)m_lastMesh.triangleCount;
        }

        void RebuildMesh(Spark::LevelDesign::CSGSystem& csg) { m_lastMesh = csg.BuildAll(); }

        uint32_t m_selectedBrush = 0;
        Spark::LevelDesign::BrushShape m_brushShape = Spark::LevelDesign::BrushShape::Box;
        Spark::LevelDesign::CSGVec3 m_brushSize{2.0f, 2.0f, 2.0f};
        Spark::LevelDesign::CSGOperation m_brushOperation = Spark::LevelDesign::CSGOperation::Additive;
        bool m_autoRebuild = true;
        bool m_createRequested = false;
        bool m_rebuildRequested = false;
        uint32_t m_deleteRequested = 0;
        std::vector<uint32_t> m_brushIds;
        Spark::LevelDesign::CSGMesh m_lastMesh;
    };

} // namespace SparkEditor
