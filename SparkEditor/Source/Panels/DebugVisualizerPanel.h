/**
 * @file DebugVisualizerPanel.h
 * @brief Debug visualization overlay panel for the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <string>
#include <vector>

namespace SparkEditor
{

    class DebugVisualizerPanel : public EditorPanel
    {
      public:
        struct DebugDrawStats
        {
            int gridLines = 0;
            int colliderShapes = 0;
            int boundingBoxes = 0;
            int navMeshPolygons = 0;
            int lightGizmos = 0;
            int audioSources = 0;
        };

        DebugVisualizerPanel();
        ~DebugVisualizerPanel() override;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        // Accessors
        bool IsGridVisible() const { return m_showGrid; }
        bool IsWireframeEnabled() const { return m_wireframeMode; }
        bool AreCollidersVisible() const { return m_showColliders; }
        bool AreBoundingBoxesVisible() const { return m_showBoundingBoxes; }
        bool AreNormalsVisible() const { return m_showNormals; }
        bool IsNavMeshVisible() const { return m_showNavMesh; }
        bool AreLightRadiiVisible() const { return m_showLightRadii; }
        bool AreAudioRadiiVisible() const { return m_showAudioRadii; }
        bool AreCameraFrustaVisible() const { return m_showCameraFrusta; }
        float GetGridSize() const { return m_gridSize; }
        float GetGridSpacing() const { return m_gridSpacing; }
        const DebugDrawStats& GetStats() const { return m_stats; }

      private:
        void RenderGridSettings();
        void RenderOverlaySettings();
        void RenderPhysicsDebug();
        void RenderNavigationDebug();
        void RenderRenderingDebug();
        void RenderAudioDebug();
        void RenderQuickToggles();
        void RenderStats();

        // Grid settings
        bool m_showGrid = true;
        float m_gridSize = 100.0f;
        float m_gridSpacing = 1.0f;
        float m_gridMajorLineEvery = 10.0f;
        float m_gridColor[4] = {0.4f, 0.4f, 0.4f, 0.5f};
        float m_gridMajorColor[4] = {0.6f, 0.6f, 0.6f, 0.7f};
        bool m_gridSnapToOrigin = true;
        int m_gridPlane = 0; // 0=XZ, 1=XY, 2=YZ

        // Wireframe / shading
        bool m_wireframeMode = false;
        bool m_showNormals = false;
        float m_normalLength = 0.5f;
        bool m_showTangents = false;
        bool m_showUVs = false;
        int m_shadingMode = 0; // 0=Lit, 1=Unlit, 2=Wireframe, 3=Normals, 4=UVs, 5=Overdraw

        // Physics debug
        bool m_showColliders = false;
        bool m_showBoundingBoxes = false;
        bool m_showCollidersFilled = false;
        float m_colliderAlpha = 0.3f;
        bool m_showPhysicsContacts = false;
        bool m_showRaycasts = false;
        bool m_showVelocityVectors = false;
        float m_velocityScale = 0.1f;

        // Navigation debug
        bool m_showNavMesh = false;
        bool m_showNavMeshBounds = false;
        bool m_showNavPaths = false;
        bool m_showNavAgents = false;
        float m_navMeshAlpha = 0.25f;

        // Audio debug
        bool m_showAudioRadii = false;
        bool m_showAudioSourceIcons = false;

        // Camera / Light debug
        bool m_showCameraFrusta = false;
        bool m_showLightRadii = false;
        bool m_showShadowCascades = false;

        // Misc debug
        bool m_showObjectOrigins = false;
        bool m_showSelectionOutline = true;
        bool m_showFPS = true;
        bool m_showDrawCalls = false;
        bool m_showTriangleCount = false;

        // Stats
        DebugDrawStats m_stats;
    };

} // namespace SparkEditor
