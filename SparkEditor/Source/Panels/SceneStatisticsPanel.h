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
            int totalEntities = 0;                                ///< Total entities in the scene.
            int activeEntities = 0;                               ///< Entities currently active and updating.
            int inactiveEntities = 0;                             ///< Entities disabled or dormant.
            std::unordered_map<std::string, int> componentCounts; ///< Per-type component counts.
        };
        EntityStats m_entityStats;

        // Render statistics
        struct RenderStats
        {
            int drawCalls = 0;            ///< GPU draw calls this frame.
            int triangleCount = 0;        ///< Total triangles rendered.
            int vertexCount = 0;          ///< Total vertices submitted.
            int shaderCount = 0;          ///< Unique shaders bound this frame.
            float textureMemoryMB = 0.0f; ///< Total texture VRAM usage (MB).
            int renderTargetCount = 0;    ///< Active render targets.
            int batchCount = 0;           ///< Batched draw call groups.
        };
        RenderStats m_renderStats;

        // Physics statistics
        struct PhysicsStats
        {
            int activeRigidBodies = 0;     ///< Dynamic rigid bodies currently simulated.
            int staticBodies = 0;          ///< Static collision bodies in the scene.
            int collisionPairs = 0;        ///< Active collision pairs this frame.
            int raycastsPerFrame = 0;      ///< Physics raycasts performed this frame.
            float simulationTimeMs = 0.0f; ///< Time spent in physics simulation (ms).
        };
        PhysicsStats m_physicsStats;

        // Memory statistics
        struct MemoryStats
        {
            float totalSceneMemoryMB = 0.0f; ///< Total memory used by the scene (MB).
            float meshMemoryMB = 0.0f;       ///< Memory used by mesh data (MB).
            float textureMemoryMB = 0.0f;    ///< Memory used by textures (MB).
            float audioMemoryMB = 0.0f;      ///< Memory used by audio buffers (MB).
            float assetCacheMemoryMB = 0.0f; ///< Memory used by the asset cache (MB).
            float scriptMemoryMB = 0.0f;     ///< Memory used by script VMs (MB).
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
