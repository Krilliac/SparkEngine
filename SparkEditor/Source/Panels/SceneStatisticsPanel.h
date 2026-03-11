/**
 * @file SceneStatisticsPanel.h
 * @brief Panel displaying real-time scene statistics and performance metrics
 * @author Spark Engine Team
 * @date 2025
 *
 * Shows entity counts, render stats, physics stats, memory usage,
 * and performance graphs for the current scene.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <array>
#include <string>
#include <unordered_map>

namespace SparkEditor
{

    /**
     * @brief Real-time scene statistics and performance metrics panel
     *
     * Displays comprehensive information about the current scene including
     * entity counts, component breakdowns, rendering statistics, physics
     * state, memory usage, and performance graphs.
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

      private:
        void RenderEntityStats();
        void RenderRenderStats();
        void RenderPhysicsStats();
        void RenderMemoryStats();
        void RenderPerformanceGraphs();
        void UpdateHistoryBuffers(float deltaTime);

        // Update frequency control
        int m_updateInterval = 10; // Update every N frames
        int m_frameCounter = 0;

        // Entity statistics
        struct EntityStats
        {
            int totalEntities = 0;
            int activeEntities = 0;
            int inactiveEntities = 0;
            std::unordered_map<std::string, int> componentCounts;
        };
        EntityStats m_entityStats;

        // Render statistics
        struct RenderStats
        {
            int drawCalls = 0;
            int triangleCount = 0;
            int vertexCount = 0;
            int shaderCount = 0;
            float textureMemoryMB = 0.0f;
            int renderTargetCount = 0;
            int batchCount = 0;
        };
        RenderStats m_renderStats;

        // Physics statistics
        struct PhysicsStats
        {
            int activeRigidBodies = 0;
            int staticBodies = 0;
            int collisionPairs = 0;
            int raycastsPerFrame = 0;
            float simulationTimeMs = 0.0f;
        };
        PhysicsStats m_physicsStats;

        // Memory statistics
        struct MemoryStats
        {
            float totalSceneMemoryMB = 0.0f;
            float meshMemoryMB = 0.0f;
            float textureMemoryMB = 0.0f;
            float audioMemoryMB = 0.0f;
            float assetCacheMemoryMB = 0.0f;
            float scriptMemoryMB = 0.0f;
        };
        MemoryStats m_memoryStats;

        // Performance history buffers
        static constexpr size_t HISTORY_SIZE = 120;
        std::array<float, HISTORY_SIZE> m_fpsHistory = {};
        std::array<float, HISTORY_SIZE> m_frameTimeHistory = {};
        std::array<float, HISTORY_SIZE> m_drawCallHistory = {};
        std::array<float, HISTORY_SIZE> m_memoryHistory = {};
        size_t m_historyIndex = 0;

        // Current performance values
        float m_currentFps = 0.0f;
        float m_currentFrameTime = 0.0f;
        float m_averageFps = 0.0f;
        float m_minFps = 999.0f;
        float m_maxFps = 0.0f;

        // Simulated stats for demonstration
        float m_simulationTime = 0.0f;

        // Collapsible section states
        bool m_showEntitySection = true;
        bool m_showRenderSection = true;
        bool m_showPhysicsSection = true;
        bool m_showMemorySection = true;
        bool m_showPerformanceSection = true;
    };

} // namespace SparkEditor
