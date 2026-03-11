/**
 * @file SceneStatsPanel.h
 * @brief Scene statistics and performance information panel
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>
#include <chrono>

namespace SparkEditor
{

    class SceneStatsPanel : public EditorPanel
    {
      public:
        SceneStatsPanel();
        ~SceneStatsPanel() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        // External data feeds
        void SetObjectCount(int total, int selected)
        {
            m_totalObjects = total;
            m_selectedObjects = selected;
        }
        void SetDrawCalls(int calls) { m_drawCalls = calls; }
        void SetTriangleCount(int tris) { m_triangleCount = tris; }
        void SetVertexCount(int verts) { m_vertexCount = verts; }
        void SetTextureMemory(size_t bytes) { m_textureMemoryBytes = bytes; }
        void SetMeshMemory(size_t bytes) { m_meshMemoryBytes = bytes; }
        void SetPhysicsBodies(int active, int sleeping)
        {
            m_activePhysicsBodies = active;
            m_sleepingPhysicsBodies = sleeping;
        }
        void SetAIAgents(int count) { m_aiAgentCount = count; }
        void SetAudioSources(int playing, int total)
        {
            m_playingAudioSources = playing;
            m_totalAudioSources = total;
        }
        void SetLightCount(int directional, int point, int spot)
        {
            m_directionalLights = directional;
            m_pointLights = point;
            m_spotLights = spot;
        }

      private:
        void RenderPerformanceSection();
        void RenderSceneSection();
        void RenderRenderingSection();
        void RenderPhysicsSection();
        void RenderAudioSection();
        void RenderMemorySection();
        void RenderFrameGraph();
        static std::string FormatBytes(size_t bytes);

        // Frame time tracking
        static constexpr size_t FRAME_HISTORY_SIZE = 120;
        float m_frameHistory[FRAME_HISTORY_SIZE] = {};
        int m_frameHistoryIndex = 0;
        float m_currentFPS = 0.0f;
        float m_avgFPS = 0.0f;
        float m_minFPS = 9999.0f;
        float m_maxFPS = 0.0f;
        float m_currentFrameTime = 0.0f;
        float m_fpsUpdateTimer = 0.0f;

        // Scene info
        int m_totalObjects = 0;
        int m_selectedObjects = 0;
        int m_totalComponents = 0;

        // Rendering
        int m_drawCalls = 0;
        int m_triangleCount = 0;
        int m_vertexCount = 0;
        int m_directionalLights = 0;
        int m_pointLights = 0;
        int m_spotLights = 0;

        // Physics
        int m_activePhysicsBodies = 0;
        int m_sleepingPhysicsBodies = 0;

        // AI
        int m_aiAgentCount = 0;

        // Audio
        int m_playingAudioSources = 0;
        int m_totalAudioSources = 0;

        // Memory
        size_t m_textureMemoryBytes = 0;
        size_t m_meshMemoryBytes = 0;
        size_t m_totalAllocatedBytes = 0;

        // Timing
        std::chrono::steady_clock::time_point m_lastUpdate;
    };

} // namespace SparkEditor
