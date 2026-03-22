/**
 * @file Physics3DPanel.h
 * @brief 3D Physics (Jolt) configuration, debug visualization, and object placement panel
 * @author Spark Engine Team
 * @date 2026
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../SceneSystem/SceneFile.h"
#include <string>
#include <cstdint>

namespace SparkEditor
{

    /**
     * @brief Editor panel for 3D Jolt Physics configuration and debugging
     *
     * Provides:
     * - World settings (gravity, timestep, substeps, deterministic mode)
     * - Debug visualization toggles (wireframes, AABBs, contacts, constraints)
     * - Physics body statistics
     * - Quick-add physics primitives to the scene
     * - Collision layer configuration
     * - Raycast testing tool
     * - Material preset management
     */
    class Physics3DPanel : public EditorPanel
    {
      public:
        Physics3DPanel();
        ~Physics3DPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        void SetScene(SceneFile* scene) { m_scene = scene; }

      private:
        void RenderWorldSettings();
        void RenderDebugVisualization();
        void RenderPhysicsStats();
        void RenderCollisionLayers();
        void RenderQuickAddPrimitives();
        void RenderMaterialPresets();
        void RenderRaycastTool();

        SceneFile* m_scene = nullptr;

        // World settings
        float m_gravity[3] = {0.0f, -9.81f, 0.0f};
        float m_timeStep = 1.0f / 60.0f;
        int m_maxSubsteps = 10;
        bool m_deterministicMode = false;

        // Debug visualization flags
        bool m_showWireframes = false;
        bool m_showAABBs = false;
        bool m_showContacts = false;
        bool m_showConstraints = false;
        bool m_showCenterOfMass = false;
        bool m_showVelocityVectors = false;

        // Collision layer configuration (3 layers: NON_MOVING, MOVING, TRIGGER)
        std::string m_layerNames[8] = {"NonMoving", "Moving",  "Trigger", "Player",
                                       "Enemy",     "Custom1", "Custom2", "Custom3"};
        bool m_layerMatrix[8][8] = {};

        // Quick-add state
        int m_quickAddShape = 0;    // 0=Box, 1=Sphere, 2=Capsule, 3=Cylinder
        int m_quickAddBodyType = 2; // 0=Static, 1=Kinematic, 2=Dynamic
        float m_quickAddMass = 1.0f;
        float m_quickAddSize[3] = {1.0f, 1.0f, 1.0f};
        float m_quickAddRadius = 0.5f;
        float m_quickAddHeight = 1.0f;

        // Material presets
        struct MaterialPreset
        {
            std::string name;
            float friction = 0.5f;
            float restitution = 0.1f;
            float density = 1.0f;
        };
        static constexpr int MAX_PRESETS = 8;
        MaterialPreset m_presets[MAX_PRESETS] = {
            {"Default", 0.5f, 0.1f, 1.0f},   {"Ice", 0.03f, 0.1f, 917.0f},    {"Rubber", 0.9f, 0.8f, 1100.0f},
            {"Wood", 0.4f, 0.3f, 600.0f},    {"Metal", 0.6f, 0.05f, 7800.0f}, {"Concrete", 0.6f, 0.0f, 2400.0f},
            {"Bouncy", 0.3f, 0.95f, 500.0f}, {"Glass", 0.4f, 0.1f, 2500.0f},
        };

        // Raycast test state
        float m_rayOrigin[3] = {0.0f, 10.0f, 0.0f};
        float m_rayDirection[3] = {0.0f, -1.0f, 0.0f};
        float m_rayMaxDist = 100.0f;
        bool m_rayHit = false;
        float m_rayHitPoint[3] = {0, 0, 0};
        float m_rayHitDistance = 0.0f;

        // Stats (refreshed each frame)
        uint32_t m_totalBodies = 0;
        uint32_t m_activeBodies = 0;
        uint32_t m_constraintCount = 0;
        float m_simTime = 0.0f;
    };

} // namespace SparkEditor
