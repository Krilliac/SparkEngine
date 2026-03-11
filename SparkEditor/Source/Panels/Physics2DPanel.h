/**
 * @file Physics2DPanel.h
 * @brief 2D Physics configuration and debug visualization panel
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <string>

namespace SparkEditor
{

    /**
     * @brief Editor panel for 2D physics configuration and debugging
     *
     * Provides:
     * - World gravity settings
     * - Collision layer matrix editor
     * - Physics body inspector for RigidBody2D/Collider2D
     * - Debug visualization toggles (AABBs, velocities, contacts, spatial hash grid)
     * - Performance stats (body count, contact count, broadphase efficiency)
     * - Raycasting test tool
     */
    class Physics2DPanel : public EditorPanel
    {
      public:
        Physics2DPanel();
        ~Physics2DPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        void SetScene(SceneFile* scene) { m_scene = scene; }

      private:
        void RenderWorldSettings();
        void RenderCollisionLayers();
        void RenderDebugVisualization();
        void RenderPhysicsStats();
        void RenderBodyInspector();
        void RenderRaycastTool();

        SceneFile* m_scene = nullptr;

        // Debug visualization flags
        bool m_showAABBs = false;
        bool m_showVelocities = false;
        bool m_showContacts = false;
        bool m_showSpatialHash = false;
        bool m_showCollisionNormals = false;

        // Gravity settings
        float m_gravity[2] = {0.0f, -9.81f};

        // Layer collision matrix (16 layers x 16 layers)
        bool m_layerMatrix[16][16] = {};

        // Layer names
        std::string m_layerNames[16] = {"Default",  "Player",    "Enemies",  "Platforms", "Projectiles", "Triggers",
                                        "Items",    "Particles", "Custom 1", "Custom 2",  "Custom 3",    "Custom 4",
                                        "Custom 5", "Custom 6",  "Custom 7", "Custom 8"};

        // Raycast test state
        float m_rayOrigin[2] = {0.0f, 0.0f};
        float m_rayDirection[2] = {1.0f, 0.0f};
        float m_rayMaxDist = 100.0f;

        // Stats
        int m_bodyCount = 0;
        int m_contactCount = 0;
        float m_stepTime = 0.0f;
    };

} // namespace SparkEditor
