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
#include <string>
#include <vector>


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

        void Render() override;

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
        bool RemoveBrushById(uint32_t brushId)
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
        void SetSelectedBrushId(uint32_t brushId)
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
        void RenderBrushCreation(Spark::LevelDesign::CSGSystem& csg);
        void RenderBrushList(Spark::LevelDesign::CSGSystem& csg);
        void RenderBuildControls(Spark::LevelDesign::CSGSystem& csg);
        void RenderStatistics();

        uint32_t m_selectedBrush = 0;
        Spark::LevelDesign::BrushShape m_brushShape = Spark::LevelDesign::BrushShape::Box;
        Spark::LevelDesign::CSGVec3 m_brushSize{2.0f, 2.0f, 2.0f};
        Spark::LevelDesign::CSGOperation m_brushOperation = Spark::LevelDesign::CSGOperation::Additive;
        bool m_autoRebuild = true;
        std::vector<uint32_t> m_brushIds;
        Spark::LevelDesign::CSGMesh m_lastMesh;
    };

} // namespace SparkEditor
