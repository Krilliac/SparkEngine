/**
 * @file EEVFXEditorSystem.h
 * @brief Visual effects / particle system editor
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a modular VFX authoring system with composable emitter modules
 * (spawn, velocity, size, color, collision, trails, etc.), multiple emitter
 * shapes, and simulation space control.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EngineEditor
{

    /// @brief A VFX module attached to an emitter
    struct VFXModuleConfig
    {
        VFXModule type = VFXModule::Spawn;
        bool enabled = true;
        float paramA = 0.0f;
        float paramB = 1.0f;
        float paramC = 0.0f;
        float paramD = 0.0f;
    };

    /// @brief A single particle emitter in a VFX asset
    struct VFXEmitter
    {
        uint32_t emitterId = 0;
        std::string name;
        EmitterShape shape = EmitterShape::Point;
        SimulationSpace space = SimulationSpace::World;
        uint32_t maxParticles = 1000;
        float duration = 5.0f;
        bool isLooping = true;
        float startDelay = 0.0f;
        std::vector<VFXModuleConfig> modules;
    };

    /// @brief A complete VFX asset (can have multiple emitters)
    struct VFXAsset
    {
        uint32_t assetId = 0;
        std::string name;
        std::string category; ///< "Fire", "Water", "Magic", "Ambient", "Impact"
        std::vector<VFXEmitter> emitters;
        bool isPlaying = false;
        float warmupTime = 0.0f;
    };

    /**
     * @brief VFX authoring system for no-code particle effects
     *
     * Manages VFX assets with modular emitters, composable behavior modules,
     * preview playback, and preset templates.
     */
    class EEVFXEditorSystem
    {
      public:
        EEVFXEditorSystem() = default;
        ~EEVFXEditorSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Asset management
        uint32_t CreateVFX(const std::string& name, const std::string& category);
        bool DeleteVFX(uint32_t assetId);

        // Emitter operations
        uint32_t AddEmitter(uint32_t assetId, const std::string& name, EmitterShape shape);
        bool RemoveEmitter(uint32_t assetId, uint32_t emitterId);
        bool AddModule(uint32_t assetId, uint32_t emitterId, VFXModule moduleType);

        // Playback
        bool PlayVFX(uint32_t assetId);
        bool StopVFX(uint32_t assetId);

        // Presets
        uint32_t CreatePresetFire(const std::string& name);
        uint32_t CreatePresetSmoke(const std::string& name);
        uint32_t CreatePresetSparks(const std::string& name);
        uint32_t CreatePresetRain(const std::string& name);
        uint32_t CreatePresetMagic(const std::string& name);

        // Queries
        size_t GetVFXCount() const { return m_assets.size(); }
        size_t GetPresetCount() const;
        std::string GetVFXListString() const;
        std::string GetVFXDetailString(uint32_t assetId) const;
        std::string GetEmitterShapeCatalog() const;

      private:
        void RegisterBuiltinPresets();
        VFXEmitter BuildFireEmitter();
        VFXEmitter BuildSmokeEmitter();
        VFXEmitter BuildSparkEmitter();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<VFXAsset> m_assets;
        uint32_t m_nextAssetId{1};
        uint32_t m_nextEmitterId{1};
        bool m_initialized{false};
    };

} // namespace EngineEditor
