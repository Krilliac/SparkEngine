/**
 * @file EEVFXEditorSystem.cpp
 * @brief Visual effects / particle system editor
 *
 * Implements modular VFX authoring with composable emitter modules,
 * preset templates, and playback control.
 */

#include "EEVFXEditorSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace EngineEditor
{

    bool EEVFXEditorSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        RegisterBuiltinPresets();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "VFX editor system initialized (%zu presets)", m_assets.size());
        return true;
    }

    void EEVFXEditorSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        for (auto& asset : m_assets)
        {
            if (!asset.isPlaying)
                continue;
            // Simulate particles for preview
            for (auto& emitter : asset.emitters)
            {
                (void)emitter;
                (void)deltaTime;
            }
        }
    }

    void EEVFXEditorSystem::Shutdown()
    {
        m_assets.clear();
        m_initialized = false;
    }

    void EEVFXEditorSystem::RenderDebugUI() {}

    uint32_t EEVFXEditorSystem::CreateVFX(const std::string& name, const std::string& category)
    {
        VFXAsset asset;
        asset.assetId = m_nextAssetId++;
        asset.name = name;
        asset.category = category;
        m_assets.push_back(std::move(asset));
        return m_assets.back().assetId;
    }

    bool EEVFXEditorSystem::DeleteVFX(uint32_t assetId)
    {
        auto it = std::find_if(m_assets.begin(), m_assets.end(),
                               [assetId](const VFXAsset& a) { return a.assetId == assetId; });
        if (it == m_assets.end())
            return false;
        m_assets.erase(it);
        return true;
    }

    uint32_t EEVFXEditorSystem::AddEmitter(uint32_t assetId, const std::string& name, EmitterShape shape)
    {
        for (auto& asset : m_assets)
        {
            if (asset.assetId == assetId)
            {
                VFXEmitter emitter;
                emitter.emitterId = m_nextEmitterId++;
                emitter.name = name;
                emitter.shape = shape;

                // Add default spawn and lifetime modules
                emitter.modules.push_back({VFXModule::Spawn, true, 10.0f, 50.0f});
                emitter.modules.push_back({VFXModule::Lifetime, true, 1.0f, 3.0f});

                asset.emitters.push_back(std::move(emitter));
                return asset.emitters.back().emitterId;
            }
        }
        return 0;
    }

    bool EEVFXEditorSystem::RemoveEmitter(uint32_t assetId, uint32_t emitterId)
    {
        for (auto& asset : m_assets)
        {
            if (asset.assetId == assetId)
            {
                auto it = std::find_if(asset.emitters.begin(), asset.emitters.end(),
                                       [emitterId](const VFXEmitter& e) { return e.emitterId == emitterId; });
                if (it == asset.emitters.end())
                    return false;
                asset.emitters.erase(it);
                return true;
            }
        }
        return false;
    }

    bool EEVFXEditorSystem::AddModule(uint32_t assetId, uint32_t emitterId, VFXModule moduleType)
    {
        for (auto& asset : m_assets)
        {
            if (asset.assetId == assetId)
            {
                for (auto& emitter : asset.emitters)
                {
                    if (emitter.emitterId == emitterId)
                    {
                        VFXModuleConfig mod;
                        mod.type = moduleType;
                        emitter.modules.push_back(mod);
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool EEVFXEditorSystem::PlayVFX(uint32_t assetId)
    {
        for (auto& asset : m_assets)
        {
            if (asset.assetId == assetId)
            {
                asset.isPlaying = true;
                return true;
            }
        }
        return false;
    }

    bool EEVFXEditorSystem::StopVFX(uint32_t assetId)
    {
        for (auto& asset : m_assets)
        {
            if (asset.assetId == assetId)
            {
                asset.isPlaying = false;
                return true;
            }
        }
        return false;
    }

    uint32_t EEVFXEditorSystem::CreatePresetFire(const std::string& name)
    {
        uint32_t id = CreateVFX(name, "Fire");
        for (auto& asset : m_assets)
        {
            if (asset.assetId == id)
            {
                asset.emitters.push_back(BuildFireEmitter());
                asset.emitters.push_back(BuildSmokeEmitter());
                asset.emitters.push_back(BuildSparkEmitter());
                break;
            }
        }
        return id;
    }

    uint32_t EEVFXEditorSystem::CreatePresetSmoke(const std::string& name)
    {
        uint32_t id = CreateVFX(name, "Ambient");
        for (auto& asset : m_assets)
        {
            if (asset.assetId == id)
            {
                VFXEmitter smoke = BuildSmokeEmitter();
                smoke.maxParticles = 500;
                asset.emitters.push_back(std::move(smoke));
                break;
            }
        }
        return id;
    }

    uint32_t EEVFXEditorSystem::CreatePresetSparks(const std::string& name)
    {
        uint32_t id = CreateVFX(name, "Impact");
        AddEmitter(id, "Sparks", EmitterShape::Point);
        for (auto& asset : m_assets)
        {
            if (asset.assetId == id)
            {
                for (auto& e : asset.emitters)
                {
                    e.maxParticles = 200;
                    e.isLooping = false;
                    e.duration = 0.5f;
                    e.modules.push_back({VFXModule::Velocity, true, 5.0f, 15.0f});
                    e.modules.push_back({VFXModule::Acceleration, true, 0.0f, -9.81f});
                    e.modules.push_back({VFXModule::Color, true, 1.0f, 0.8f, 0.2f, 1.0f});
                    e.modules.push_back({VFXModule::Size, true, 0.02f, 0.08f});
                }
                break;
            }
        }
        return id;
    }

    uint32_t EEVFXEditorSystem::CreatePresetRain(const std::string& name)
    {
        uint32_t id = CreateVFX(name, "Weather");
        AddEmitter(id, "Raindrops", EmitterShape::Box);
        for (auto& asset : m_assets)
        {
            if (asset.assetId == id)
            {
                for (auto& e : asset.emitters)
                {
                    e.maxParticles = 5000;
                    e.duration = 0.0f;
                    e.modules.push_back({VFXModule::Velocity, true, 0.0f, -8.0f, 0.0f, -12.0f});
                    e.modules.push_back({VFXModule::Size, true, 0.005f, 0.015f});
                    e.modules.push_back({VFXModule::Color, true, 0.7f, 0.8f, 0.9f, 0.6f});
                    e.modules.push_back({VFXModule::Collision, true, 0.0f, 0.0f});
                }
                break;
            }
        }
        return id;
    }

    uint32_t EEVFXEditorSystem::CreatePresetMagic(const std::string& name)
    {
        uint32_t id = CreateVFX(name, "Magic");
        AddEmitter(id, "Orbs", EmitterShape::Sphere);
        AddEmitter(id, "Trails", EmitterShape::Point);
        for (auto& asset : m_assets)
        {
            if (asset.assetId == id)
            {
                for (auto& e : asset.emitters)
                {
                    e.modules.push_back({VFXModule::Orbit, true, 2.0f, 1.0f});
                    e.modules.push_back({VFXModule::Color, true, 0.3f, 0.5f, 1.0f, 0.8f});
                    e.modules.push_back({VFXModule::Light, true, 1.0f, 3.0f});
                    if (e.name == "Trails")
                        e.modules.push_back({VFXModule::Trail, true, 0.5f, 0.0f});
                }
                break;
            }
        }
        return id;
    }

    size_t EEVFXEditorSystem::GetPresetCount() const
    {
        size_t count = 0;
        for (const auto& a : m_assets)
        {
            if (!a.category.empty())
                count++;
        }
        return count;
    }

    std::string EEVFXEditorSystem::GetVFXListString() const
    {
        std::string s = "=== VFX Assets ===\n";
        for (const auto& a : m_assets)
        {
            s += "  [" + std::to_string(a.assetId) + "] " + a.name;
            s += " (" + a.category + ", " + std::to_string(a.emitters.size()) + " emitters";
            if (a.isPlaying)
                s += ", PLAYING";
            s += ")\n";
        }
        if (m_assets.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EEVFXEditorSystem::GetVFXDetailString(uint32_t assetId) const
    {
        for (const auto& a : m_assets)
        {
            if (a.assetId == assetId)
            {
                std::string s = "=== VFX: " + a.name + " ===\n";
                s += "Category: " + a.category + "\n";
                s += "Emitters: " + std::to_string(a.emitters.size()) + "\n";
                for (const auto& e : a.emitters)
                {
                    s += "  [" + std::to_string(e.emitterId) + "] " + e.name;
                    s += " (shape=" + std::to_string(static_cast<int>(e.shape));
                    s += ", max=" + std::to_string(e.maxParticles);
                    s += ", modules=" + std::to_string(e.modules.size());
                    s += ")\n";
                }
                return s;
            }
        }
        return "VFX not found";
    }

    std::string EEVFXEditorSystem::GetEmitterShapeCatalog() const
    {
        return "Emitter Shapes: Point, Sphere, Hemisphere, Cone, Box, Ring, Mesh, Edge\n";
    }

    VFXEmitter EEVFXEditorSystem::BuildFireEmitter()
    {
        VFXEmitter e;
        e.emitterId = m_nextEmitterId++;
        e.name = "Flames";
        e.shape = EmitterShape::Cone;
        e.maxParticles = 300;
        e.duration = 0.0f;
        e.modules = {
            {VFXModule::Spawn, true, 30.0f, 60.0f},
            {VFXModule::Lifetime, true, 0.5f, 1.5f},
            {VFXModule::Velocity, true, 0.0f, 3.0f},
            {VFXModule::Size, true, 0.1f, 0.5f},
            {VFXModule::Color, true, 1.0f, 0.5f, 0.1f, 0.8f},
            {VFXModule::Noise, true, 0.3f, 1.0f},
            {VFXModule::Light, true, 1.0f, 5.0f},
        };
        return e;
    }

    VFXEmitter EEVFXEditorSystem::BuildSmokeEmitter()
    {
        VFXEmitter e;
        e.emitterId = m_nextEmitterId++;
        e.name = "Smoke";
        e.shape = EmitterShape::Cone;
        e.maxParticles = 200;
        e.duration = 0.0f;
        e.modules = {
            {VFXModule::Spawn, true, 10.0f, 20.0f},           {VFXModule::Lifetime, true, 2.0f, 4.0f},
            {VFXModule::Velocity, true, 0.5f, 2.0f},          {VFXModule::Size, true, 0.3f, 1.5f},
            {VFXModule::Color, true, 0.3f, 0.3f, 0.3f, 0.4f}, {VFXModule::Rotation, true, 0.0f, 360.0f},
        };
        return e;
    }

    VFXEmitter EEVFXEditorSystem::BuildSparkEmitter()
    {
        VFXEmitter e;
        e.emitterId = m_nextEmitterId++;
        e.name = "Embers";
        e.shape = EmitterShape::Point;
        e.maxParticles = 100;
        e.duration = 0.0f;
        e.modules = {
            {VFXModule::Spawn, true, 5.0f, 15.0f},   {VFXModule::Lifetime, true, 1.0f, 3.0f},
            {VFXModule::Velocity, true, 1.0f, 4.0f}, {VFXModule::Acceleration, true, 0.0f, 0.5f},
            {VFXModule::Size, true, 0.01f, 0.04f},   {VFXModule::Color, true, 1.0f, 0.7f, 0.2f, 1.0f},
            {VFXModule::Light, true, 0.2f, 1.0f},
        };
        return e;
    }

    void EEVFXEditorSystem::RegisterBuiltinPresets()
    {
        CreatePresetFire("VFX_Fire");
        CreatePresetSmoke("VFX_Smoke");
        CreatePresetSparks("VFX_Sparks");
        CreatePresetRain("VFX_Rain");
        CreatePresetMagic("VFX_Magic");
    }

} // namespace EngineEditor
