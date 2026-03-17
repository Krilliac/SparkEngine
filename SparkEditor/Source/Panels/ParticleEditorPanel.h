/**
 * @file ParticleEditorPanel.h
 * @brief Particle system editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;

namespace SparkEditor
{

    /**
     * @brief Panel for visually authoring particle emitter configurations
     *
     * Exposes emission rate, lifetime, shape, color gradient, size curves,
     * physics, and rendering settings for ParticleEmitterDesc.
     */
    class ParticleEditorPanel : public EditorPanel
    {
      public:
        ParticleEditorPanel();
        ~ParticleEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        void RenderEmissionSettings();
        void RenderShapeSettings();
        void RenderAppearanceSettings();
        void RenderPhysicsSettings();
        void RenderRenderingSettings();

        // Emitter configuration (mirrors ParticleEmitterDesc)
        char m_effectName[128] = "NewEffect";
        float m_emissionRate = 10.0f;
        int m_maxParticles = 1000;
        int m_burstCount = 0;

        int m_shape = 0; // 0=Point, 1=Sphere, 2=Cone, 3=Box, 4=Circle
        float m_shapeRadius = 1.0f;
        XMFLOAT3 m_shapeExtents = {1.0f, 1.0f, 1.0f};
        float m_coneAngle = 45.0f;

        float m_lifetimeMin = 1.0f, m_lifetimeMax = 2.0f;
        float m_speedMin = 1.0f, m_speedMax = 5.0f;
        float m_sizeMin = 0.1f, m_sizeMax = 0.5f;

        XMFLOAT4 m_startColor = {1.0f, 1.0f, 1.0f, 1.0f};
        XMFLOAT4 m_endColor = {1.0f, 1.0f, 1.0f, 0.0f};

        float m_gravityMultiplier = 0.0f;
        float m_drag = 0.0f;

        int m_blendMode = 0; // 0=Additive, 1=AlphaBlend, 2=Multiply
        int m_space = 0;     // 0=World, 1=Local
        bool m_loop = true;
        bool m_playOnAwake = true;
        bool m_prewarm = false;
    };

} // namespace SparkEditor
