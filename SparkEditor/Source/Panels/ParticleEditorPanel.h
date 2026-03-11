/**
 * @file ParticleEditorPanel.h
 * @brief Visual particle system editor for Spark Engine
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a visual editor for designing and previewing particle effects.
 * Supports emitter configuration, burst emission, color gradients over
 * lifetime, size curves, velocity fields, and real-time 3D preview.
 */

#pragma once

#include <vector>
#include <string>
#include <unordered_map>
#include <memory>
#include <functional>
#include <algorithm>

#include <imgui.h>

#include "../Core/EditorPanel.h"

namespace SparkEditor
{

    // =========================================================================
    // Particle effect data model (editor-side representation)
    // =========================================================================

    /**
     * @brief Keyframe for a float curve (size over lifetime, speed over lifetime, etc.)
     */
    struct ParticleCurveKey
    {
        float time = 0.0f;  ///< Normalized time [0, 1] over particle lifetime
        float value = 1.0f; ///< Value at this keyframe
    };

    /**
     * @brief Keyframe for a color gradient (color over lifetime)
     */
    struct ParticleColorKey
    {
        float time = 0.0f;        ///< Normalized time [0, 1]
        ImVec4 color{1, 1, 1, 1}; ///< RGBA color at this keyframe
    };

    /**
     * @brief Shape from which particles are emitted
     */
    enum class EmitterShape
    {
        Point,      ///< All particles spawn at emitter origin
        Sphere,     ///< Spawn on or within a sphere
        Hemisphere, ///< Spawn on or within upper hemisphere
        Cone,       ///< Spawn in a cone direction
        Box,        ///< Spawn within a box volume
        Circle,     ///< Spawn on a 2D circle (XZ plane)
        Edge        ///< Spawn along a line segment
    };

    /**
     * @brief Blend mode for particle rendering
     */
    enum class ParticleBlendMode
    {
        Additive,     ///< Add to background (fire, glow, sparks)
        AlphaBlend,   ///< Standard alpha blending (smoke, dust)
        Multiply,     ///< Darken background (shadows, fog)
        Premultiplied ///< Pre-multiplied alpha (UI particles)
    };

    /**
     * @brief Configuration for a single burst emission event
     */
    struct ParticleBurst
    {
        float time = 0.0f;     ///< Time offset from emitter start (seconds)
        int minCount = 10;     ///< Minimum particles in burst
        int maxCount = 10;     ///< Maximum particles in burst
        int cycleCount = 1;    ///< Number of times to repeat (0 = infinite)
        float interval = 1.0f; ///< Interval between repeats (seconds)
    };

    /**
     * @brief Sub-emitter trigger event
     */
    enum class SubEmitterTrigger
    {
        Birth,    ///< Spawn sub-emitter when particle is born
        Death,    ///< Spawn sub-emitter when particle dies
        Collision ///< Spawn sub-emitter on particle collision
    };

    /**
     * @brief Reference to a sub-emitter
     */
    struct SubEmitterEntry
    {
        SubEmitterTrigger trigger = SubEmitterTrigger::Death;
        std::string emitterName; ///< Name of the sub-emitter preset
        int maxSpawn = 5;        ///< Max particles to inherit from sub-emitter
    };

    /**
     * @brief Complete emitter module configuration
     */
    struct ParticleEmitterConfig
    {
        /// General
        std::string name = "New Emitter";
        bool enabled = true;
        float duration = 5.0f;   ///< Total duration of the effect (seconds)
        bool looping = true;     ///< Whether the emitter loops
        float startDelay = 0.0f; ///< Delay before first emission (seconds)
        int maxParticles = 1000; ///< Maximum alive particles for this emitter

        /// Emission
        float emissionRate = 50.0f; ///< Particles per second (continuous)
        std::vector<ParticleBurst> bursts;

        /// Shape
        EmitterShape shape = EmitterShape::Cone;
        float shapeRadius = 1.0f;     ///< Radius for Sphere/Hemisphere/Cone/Circle
        float shapeConeAngle = 25.0f; ///< Half-angle for Cone shape (degrees)
        float shapeBoxWidth = 1.0f;   ///< Box half-extent X
        float shapeBoxHeight = 1.0f;  ///< Box half-extent Y
        float shapeBoxDepth = 1.0f;   ///< Box half-extent Z
        bool emitFromShell = false;   ///< Emit from surface only (not volume)

        /// Initial values
        float startLifetimeMin = 1.0f;
        float startLifetimeMax = 3.0f;
        float startSpeedMin = 2.0f;
        float startSpeedMax = 5.0f;
        float startSizeMin = 0.1f;
        float startSizeMax = 0.3f;
        float startRotationMin = 0.0f;   ///< Degrees
        float startRotationMax = 360.0f; ///< Degrees
        ImVec4 startColorMin{1, 1, 1, 1};
        ImVec4 startColorMax{1, 1, 1, 1};

        /// Over-lifetime curves
        std::vector<ParticleCurveKey> sizeOverLifetime = {{0.0f, 1.0f}, {1.0f, 0.0f}};
        std::vector<ParticleCurveKey> speedOverLifetime = {{0.0f, 1.0f}, {1.0f, 0.5f}};
        std::vector<ParticleCurveKey> rotationSpeedOverLifetime;
        std::vector<ParticleColorKey> colorOverLifetime = {
            {0.0f, {1, 1, 1, 1}}, {0.8f, {1, 1, 1, 1}}, {1.0f, {1, 1, 1, 0}}};

        /// Physics
        float gravityModifier = 0.0f; ///< Multiplier for world gravity
        float dragCoefficient = 0.0f; ///< Air resistance
        bool worldSpace = true;       ///< Simulate in world space vs local space

        /// Rendering
        ParticleBlendMode blendMode = ParticleBlendMode::Additive;
        std::string texturePath;          ///< Sprite sheet or single texture
        int textureSheetRows = 1;         ///< Rows in sprite sheet
        int textureSheetColumns = 1;      ///< Columns in sprite sheet
        bool animateTextureSheet = false; ///< Cycle through sheet frames
        bool billboardMode = true;        ///< Always face camera
        int renderSortOrder = 0;          ///< Render queue sort order

        /// Sub-emitters
        std::vector<SubEmitterEntry> subEmitters;

        /// Collision
        bool enableCollision = false;
        float collisionBounce = 0.5f;       ///< Coefficient of restitution [0, 1]
        float collisionLifetimeLoss = 0.1f; ///< Fraction of lifetime lost on bounce
    };

    /**
     * @brief A complete particle effect composed of one or more emitters
     */
    struct ParticleEffectAsset
    {
        std::string name = "Untitled Effect";
        std::string filePath;
        std::vector<ParticleEmitterConfig> emitters;
        bool isModified = false;
    };

    // =========================================================================
    // ParticleEditorPanel
    // =========================================================================

    /**
     * @brief Visual particle system editor panel
     *
     * Provides a comprehensive interface for designing particle effects:
     * - Emitter list with add/remove/duplicate
     * - Per-emitter property inspector with categorized sections
     * - Curve editor for size/speed/rotation over lifetime
     * - Color gradient editor for color over lifetime
     * - Burst emission timeline
     * - Sub-emitter configuration
     * - Real-time 3D preview with playback controls
     * - Effect library with presets (fire, smoke, sparks, blood, muzzle flash)
     */
    class ParticleEditorPanel : public EditorPanel
    {
      public:
        ParticleEditorPanel();
        ~ParticleEditorPanel() override;

        // EditorPanel interface
        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;
        std::string GetTypeName() const override { return "ParticleEditorPanel"; }

        /**
         * @brief Create a new empty particle effect
         */
        void NewEffect();

        /**
         * @brief Load a particle effect from file
         * @param filePath Path to the .sparkfx file
         * @return true if load succeeded
         */
        bool LoadEffect(const std::string& filePath);

        /**
         * @brief Save the current effect to file
         * @param filePath Path to save to (empty = use current path)
         * @return true if save succeeded
         */
        bool SaveEffect(const std::string& filePath = "");

        /**
         * @brief Apply a built-in preset to the current effect
         * @param presetName Name of the preset (e.g. "Fire", "Smoke", "MuzzleFlash")
         */
        void ApplyPreset(const std::string& presetName);

        /**
         * @brief Get the current effect being edited
         * @return Pointer to the current effect, or nullptr if none
         */
        const ParticleEffectAsset* GetCurrentEffect() const { return m_currentEffect.get(); }

      private:
        // Render sub-sections
        void RenderToolbar();
        void RenderEmitterList();
        void RenderEmitterProperties();
        void RenderCurveEditor(const char* label, std::vector<ParticleCurveKey>& curve);
        void RenderColorGradient(const char* label, std::vector<ParticleColorKey>& gradient);
        void RenderBurstList(std::vector<ParticleBurst>& bursts);
        void RenderSubEmitterList(std::vector<SubEmitterEntry>& subEmitters);
        void RenderPreviewControls();
        void RenderPresetLibrary();

        // Preset loading
        void InitializePresets();
        ParticleEmitterConfig CreateFirePreset() const;
        ParticleEmitterConfig CreateSmokePreset() const;
        ParticleEmitterConfig CreateSparksPreset() const;
        ParticleEmitterConfig CreateBloodPreset() const;
        ParticleEmitterConfig CreateMuzzleFlashPreset() const;
        ParticleEmitterConfig CreateExplosionPreset() const;
        ParticleEmitterConfig CreateRainPreset() const;
        ParticleEmitterConfig CreateDustPreset() const;

      private:
        std::unique_ptr<ParticleEffectAsset> m_currentEffect;
        int m_selectedEmitterIndex = -1;

        // Preview state
        bool m_previewPlaying = true;
        float m_previewTime = 0.0f;
        float m_previewSpeed = 1.0f;
        bool m_showGrid = true;
        bool m_showBounds = false;
        float m_previewZoom = 5.0f;

        // Curve editor state
        int m_selectedCurveKeyIndex = -1;
        bool m_isDraggingCurveKey = false;

        // Preset library
        std::unordered_map<std::string, ParticleEmitterConfig> m_presets;
        bool m_showPresetLibrary = false;
    };

} // namespace SparkEditor
