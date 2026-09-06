/**
 * @file SceneStatisticsPanel.h
 * @brief Panel displaying real scene statistics and performance metrics
 * @author Spark Engine Team
 * @date 2025
 *
 * Every number shown here is measured: entity and component counts come from the
 * editor World's registry, render counts from the viewport's last reported
 * WorldBasicRenderStats, physics counts from the running PhysicsSystem, memory
 * from the OS, and FPS from the measured frame delta. Sections with no live
 * source say so instead of showing a placeholder value.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "Graphics/WorldBasicRenderer.h"
#include <array>
#include <string>
#include <unordered_map>

class World;

namespace SparkEditor
{

    /**
     * @brief Scene statistics and performance metrics panel
     */
    class SceneStatisticsPanel : public EditorPanel
    {
      public:
        SceneStatisticsPanel();
        ~SceneStatisticsPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "SceneStatisticsPanel"; }

        /// @brief Point the panel at the document World whose entities it counts.
        void SetWorld(::World* world) { m_world = world; }

        /// @brief Feed the viewport's last render stats; enables the render section.
        void SetRenderStats(const Spark::WorldBasicRenderStats& stats)
        {
            m_renderStats = stats;
            m_hasRenderStats = true;
        }

        /// @brief Whether a World is wired in and entity counts are real.
        bool IsWorldConnected() const { return m_world != nullptr; }
        /// @brief Whether the viewport has reported render stats at least once.
        bool HasRenderStats() const { return m_hasRenderStats; }

        /// @brief Entities counted in the connected World (0 when unconnected).
        int GetTotalEntities() const { return m_totalEntities; }
        /// @brief Per-component-type counts read from the World registry storage.
        const std::unordered_map<std::string, int>& GetComponentCounts() const { return m_componentCounts; }
        /// @brief Frames per second derived from the measured frame delta.
        float GetCurrentFps() const { return m_currentFps; }

        /// @brief Re-read every live source immediately (called on the update interval).
        void CollectStats();

      private:
        void RenderEntityStats();
        void RenderRenderStats();
        void RenderPhysicsStats();
        void RenderMemoryStats();
        void RenderPerformanceGraphs();
        void UpdateHistoryBuffers(float deltaTime);

        // Update frequency control
        int m_updateInterval = 10; // Collect every N frames
        int m_frameCounter = 0;

        // Document World (non-owning); nullptr until the editor wires one in.
        ::World* m_world = nullptr;

        // Entity statistics, measured from the World registry.
        int m_totalEntities = 0;
        std::unordered_map<std::string, int> m_componentCounts;

        // Render statistics, reported by the viewport renderer.
        Spark::WorldBasicRenderStats m_renderStats;
        bool m_hasRenderStats = false;

        // Physics statistics, read from the running PhysicsSystem.
        struct PhysicsStats
        {
            int activeRigidBodies = 0;
            int totalRigidBodies = 0;
            int collisionPairs = 0;
            int raycastsPerFrame = 0;
            float simulationTimeMs = 0.0f;
        };
        PhysicsStats m_physicsStats;
        bool m_hasPhysicsStats = false;

        // Process memory reported by the OS.
        float m_processMemoryMB = 0.0f;
        bool m_hasProcessMemory = false;

        // Performance history buffers
        static constexpr size_t HISTORY_SIZE = 120;
        std::array<float, HISTORY_SIZE> m_fpsHistory = {};
        std::array<float, HISTORY_SIZE> m_frameTimeHistory = {};
        std::array<float, HISTORY_SIZE> m_drawCallHistory = {};
        size_t m_historyIndex = 0;

        // Current performance values
        float m_currentFps = 0.0f;
        float m_currentFrameTime = 0.0f;
        float m_averageFps = 0.0f;
        float m_minFps = 0.0f;
        float m_maxFps = 0.0f;

        // Collapsible section states
        bool m_showEntitySection = true;
        bool m_showRenderSection = true;
        bool m_showPhysicsSection = true;
        bool m_showMemorySection = true;
        bool m_showPerformanceSection = true;
    };

} // namespace SparkEditor
