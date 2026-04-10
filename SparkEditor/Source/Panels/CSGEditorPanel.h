/**
 * @file CSGEditorPanel.h
 * @brief Editor panel for constructive solid geometry (CSG) level design
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides an ImGui interface for creating CSG brushes, selecting boolean
 * operations, and previewing the resulting mesh in real time.
 *
 * The panel is split into a public API layer (creation / deletion / rebuild,
 * auto-rebuild toggle, selection, mesh query, statistics) that is fully
 * testable without ImGui, and an ImGui-rendering layer (`Render()`) that
 * wires buttons and sliders to those public methods. The CI test binary
 * exercises only the public API — rendering is covered by live editor
 * testing.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Engine/LevelDesign/CSGSystem.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

// The panel renders with Dear ImGui when available, and compiles as a
// no-op in headless / test builds where ImGui is not linked.
#if __has_include(<imgui.h>)
#include <imgui.h>
#define SPARK_CSG_PANEL_HAS_IMGUI 1
#else
#define SPARK_CSG_PANEL_HAS_IMGUI 0
#endif

namespace SparkEditor
{

    /**
     * @brief Editor panel for CSG-based level greyboxing
     *
     * Wraps `Spark::LevelDesign::CSGSystem` with editor-side bookkeeping:
     * tracks the brushes that were created through this panel so the UI
     * can list them in creation order, maintains a selected-brush cursor,
     * caches the last built mesh for the statistics widget, and offers an
     * "auto-rebuild on edit" toggle.
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
            m_brushIds.clear();
            return true;
        }

        void Update(float deltaTime) override { (void)deltaTime; }

        void Render() override
        {
            auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
            // ImGui widgets call into the public API below. Placeholder
            // implementation — live editor testing provides the real UI.
            RenderBrushCreation(csg);
            RenderBrushList(csg);
            RenderBuildControls(csg);
            RenderStatistics();
        }

        void Shutdown() override { m_brushIds.clear(); }

        std::string GetTypeName() const override { return "CSGEditorPanel"; }

        // ====================================================================
        // Public API — callable from tests and from ImGui button callbacks
        // ====================================================================

        /**
         * @brief Create a brush with the given parameters and track it.
         *
         * Calls `CSGSystem::CreateBrush`, applies the boolean operation,
         * marks the new brush as selected, and triggers an auto-rebuild
         * if enabled. Returns the brush ID (0 on failure).
         */
        uint32_t CreateBrush(Spark::LevelDesign::BrushShape shape, const Spark::LevelDesign::CSGVec3& size,
                             Spark::LevelDesign::CSGOperation op = Spark::LevelDesign::CSGOperation::Additive)
        {
            auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
            const uint32_t id = csg.CreateBrush(shape, size);
            if (id == 0)
                return 0;
            csg.SetBrushOperation(id, op);
            m_brushIds.push_back(id);
            m_selectedBrush = id;
            if (m_autoRebuild)
                RebuildMesh();
            return id;
        }

        /**
         * @brief Remove a brush by ID and untrack it.
         *
         * Returns true if the brush was known to this panel. Triggers an
         * auto-rebuild if enabled. Clears the selection cursor if the
         * removed brush was selected.
         */
        bool DeleteBrush(uint32_t brushId)
        {
            auto it = std::find(m_brushIds.begin(), m_brushIds.end(), brushId);
            if (it == m_brushIds.end())
                return false;

            auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
            csg.RemoveBrush(brushId);
            m_brushIds.erase(it);
            if (m_selectedBrush == brushId)
                m_selectedBrush = m_brushIds.empty() ? 0 : m_brushIds.back();
            if (m_autoRebuild)
                RebuildMesh();
            return true;
        }

        /**
         * @brief Rebuild the combined CSG mesh from all tracked brushes.
         *
         * Stores the result in `m_lastMesh` so the statistics widget can
         * read vertex/triangle counts without re-running BuildAll.
         */
        void RebuildMesh()
        {
            auto& csg = Spark::LevelDesign::CSGSystem::GetInstance();
            m_lastMesh = csg.BuildAll();
        }

        /**
         * @brief Set the "selected" brush by ID. A value of 0 clears the
         *        selection cursor. Unknown IDs are ignored.
         */
        void SelectBrush(uint32_t brushId)
        {
            if (brushId == 0)
            {
                m_selectedBrush = 0;
                return;
            }
            if (std::find(m_brushIds.begin(), m_brushIds.end(), brushId) != m_brushIds.end())
                m_selectedBrush = brushId;
        }

        /** @brief Brush currently under the selection cursor (0 if none). */
        uint32_t GetSelectedBrush() const { return m_selectedBrush; }

        /** @brief Brushes created through this panel, in creation order. */
        const std::vector<uint32_t>& GetBrushes() const { return m_brushIds; }

        /** @brief Number of tracked brushes. */
        uint32_t GetBrushCount() const { return static_cast<uint32_t>(m_brushIds.size()); }

        /** @brief Last-built mesh snapshot (vertex / triangle counts). */
        const Spark::LevelDesign::CSGMesh& GetLastMesh() const { return m_lastMesh; }

        /** @brief Enable or disable auto-rebuild on edit. */
        void SetAutoRebuildEnabled(bool enabled) { m_autoRebuild = enabled; }

        /** @brief Query the auto-rebuild setting. */
        bool IsAutoRebuildEnabled() const { return m_autoRebuild; }

        /** @brief Default shape used by the ImGui "Create" button. */
        void SetDefaultBrushShape(Spark::LevelDesign::BrushShape shape) { m_brushShape = shape; }
        Spark::LevelDesign::BrushShape GetDefaultBrushShape() const { return m_brushShape; }

        /** @brief Default size used by the ImGui "Create" button. */
        void SetDefaultBrushSize(const Spark::LevelDesign::CSGVec3& size) { m_brushSize = size; }
        const Spark::LevelDesign::CSGVec3& GetDefaultBrushSize() const { return m_brushSize; }

        /** @brief Default operation used by the ImGui "Create" button. */
        void SetDefaultBrushOperation(Spark::LevelDesign::CSGOperation op) { m_brushOperation = op; }
        Spark::LevelDesign::CSGOperation GetDefaultBrushOperation() const { return m_brushOperation; }

      private:
#if SPARK_CSG_PANEL_HAS_IMGUI
        void RenderBrushCreation(Spark::LevelDesign::CSGSystem& /*csg*/)
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

        void RenderBrushList(Spark::LevelDesign::CSGSystem& csg)
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
                DeleteBrush(toDelete);
            }
            ImGui::Spacing();
        }

        void RenderBuildControls(Spark::LevelDesign::CSGSystem& /*csg*/)
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

        void RenderStatistics()
        {
            ImGui::TextUnformatted("Statistics");
            ImGui::Separator();
            ImGui::Text("Brushes:   %u", static_cast<unsigned>(m_brushIds.size()));
            ImGui::Text("Triangles: %u", static_cast<unsigned>(m_lastMesh.triangleCount));
            ImGui::Text("Vertices:  %u", static_cast<unsigned>(m_lastMesh.vertices.size()));
            ImGui::Text("Selected:  %u", static_cast<unsigned>(m_selectedBrush));
        }
#else
        void RenderBrushCreation(Spark::LevelDesign::CSGSystem& /*csg*/) {}
        void RenderBrushList(Spark::LevelDesign::CSGSystem& /*csg*/) {}
        void RenderBuildControls(Spark::LevelDesign::CSGSystem& /*csg*/) {}
        void RenderStatistics() {}
#endif

        uint32_t m_selectedBrush = 0;
        Spark::LevelDesign::BrushShape m_brushShape = Spark::LevelDesign::BrushShape::Box;
        Spark::LevelDesign::CSGVec3 m_brushSize{2.0f, 2.0f, 2.0f};
        Spark::LevelDesign::CSGOperation m_brushOperation = Spark::LevelDesign::CSGOperation::Additive;
        bool m_autoRebuild = true;
        std::vector<uint32_t> m_brushIds;
        Spark::LevelDesign::CSGMesh m_lastMesh;
    };

} // namespace SparkEditor
