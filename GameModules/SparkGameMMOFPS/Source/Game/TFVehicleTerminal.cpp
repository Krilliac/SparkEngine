/**
 * @file TFVehicleTerminal.cpp
 * @brief Vehicle terminal prop spawner (see TFVehicleTerminal.h).
 *
 * Follows TFRegionSystem::UpdateCaptureVisuals to the letter: spawn once when
 * the ECS world + terrain + data tables are all live, then cheap periodic
 * owner-retint via TFRegionSystem::OwnerOf (the same
 * FactionStructureMaterial scheme the capture banners use).
 */
#include "Game/TFVehicleTerminal.h"

#include "Data/TFDataTables.h"
#include "Game/TFVisualUtils.h" // FactionStructureMaterial (owner tint)
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h" // TerrainHeightAt for pad placement

#include "Engine/ECS/Components.h" // Transform, MeshRenderer
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <cmath>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;
        constexpr float kRetintSec = 0.5f;      // ownership flips are 1 Hz server-side
        constexpr float kClutterOffsetM = 2.4f; // ammo printer beside the console

        constexpr const char* kConsoleMesh = "Assets/Models/MMOFPS/buildings/vehicle_terminal.obj";
        constexpr const char* kClutterMesh = "Assets/Models/MMOFPS/props/prop_ammo_printer.obj";

    } // namespace

    TFVehicleTerminal::TFVehicleTerminal() = default;
    TFVehicleTerminal::~TFVehicleTerminal()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFVehicleTerminal::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFVehicleTerminal initialized");
        return true;
    }

    void TFVehicleTerminal::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return; // viewer-only decor; the dedicated server never spawns it

        if (!m_spawned)
            SpawnPads();
        if (!m_spawned)
            return;

        m_retintAccum += deltaTime;
        if (m_retintAccum >= kRetintSec)
        {
            m_retintAccum = 0.0f;
            RetintPads();
        }
    }

    void TFVehicleTerminal::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFVehicleTerminal::Shutdown()
    {
        if (!m_initialized)
            return;
        DestroyPads();
        m_initialized = false;
    }

    void TFVehicleTerminal::SpawnPads()
    {
        // Same readiness bar as the capture landmarks: ECS world + terrain + data.
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || !m_ctx->world || !m_ctx->data || !m_ctx->data->IsLoaded())
            return;
        const auto& regions = m_ctx->data->GetContinent().regions;
        if (regions.empty())
            return;

        for (const RegionDef& r : regions)
        {
            if (!r.vehicleTerminal.has_value())
                continue;

            const float x = (*r.vehicleTerminal)[0];
            const float z = (*r.vehicleTerminal)[1];
            const float y = m_ctx->world->TerrainHeightAt(x, z);
            // Console faces the region center (where the pad drops vehicles).
            const float yawDeg = std::atan2(r.centerX - x, r.centerZ - z) * kRadToDeg;
            const FactionId owner = m_ctx->regions ? m_ctx->regions->OwnerOf(r.id) : FactionId::None;

            Pad pad;
            pad.region = r.id;
            pad.lastOwner = static_cast<int>(owner);

            const auto console = world->CreateEntity("TF_VehTerminal");
            Transform& ct = world->AddComponent<Transform>(console);
            ct.position = {x, y, z};
            ct.rotation.y = yawDeg;
            MeshRenderer& cmr = world->AddComponent<MeshRenderer>(console);
            cmr.meshPath = kConsoleMesh;
            cmr.materialPath = FactionStructureMaterial(*m_ctx, owner);
            cmr.castShadows = true;
            cmr.emissive = 0.35f; // powered console reads at range (banner precedent)
            pad.console = static_cast<uint32_t>(console);

            // Side clutter: ammo printer, offset perpendicular to the facing.
            const float side = yawDeg * (1.0f / kRadToDeg) + 1.5707963f;
            const float cx = x + std::sin(side) * kClutterOffsetM;
            const float cz = z + std::cos(side) * kClutterOffsetM;
            const auto clutter = world->CreateEntity("TF_VehTerminalPrinter");
            Transform& pt = world->AddComponent<Transform>(clutter);
            pt.position = {cx, m_ctx->world->TerrainHeightAt(cx, cz), cz};
            pt.rotation.y = yawDeg;
            MeshRenderer& pmr = world->AddComponent<MeshRenderer>(clutter);
            pmr.meshPath = kClutterMesh;
            pmr.materialPath = FactionStructureMaterial(*m_ctx, owner);
            pmr.castShadows = true;
            pad.clutter = static_cast<uint32_t>(clutter);

            m_pads.push_back(pad);
        }

        m_spawned = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] spawned %zu vehicle terminal pads", m_pads.size());
    }

    void TFVehicleTerminal::RetintPads()
    {
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || !m_ctx->regions)
            return;
        auto& registry = world->GetRegistry();

        for (Pad& pad : m_pads)
        {
            const FactionId owner = m_ctx->regions->OwnerOf(pad.region);
            if (static_cast<int>(owner) == pad.lastOwner)
                continue;
            pad.lastOwner = static_cast<int>(owner);
            const std::string& mat = FactionStructureMaterial(*m_ctx, owner);
            for (const uint32_t ent : {pad.console, pad.clutter})
            {
                if (ent == 0u)
                    continue;
                const auto e = static_cast<EntityID>(ent);
                if (!registry.valid(e))
                    continue;
                if (MeshRenderer* mr = registry.try_get<MeshRenderer>(e))
                    mr->materialPath = mat;
            }
        }
    }

    void TFVehicleTerminal::DestroyPads()
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (world)
        {
            for (const Pad& pad : m_pads)
            {
                for (const uint32_t ent : {pad.console, pad.clutter})
                {
                    const auto e = static_cast<EntityID>(ent);
                    if (ent != 0u && world->GetRegistry().valid(e))
                        world->DestroyEntity(e);
                }
            }
        }
        m_pads.clear();
        m_spawned = false;
    }

    void TFVehicleTerminal::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Vehicle Terminals"))
            return;
        ImGui::Text("pads: %zu  spawned: %d", m_pads.size(), m_spawned ? 1 : 0);
        for (const Pad& pad : m_pads)
            ImGui::Text("region %u owner=%s console=%u printer=%u", static_cast<unsigned>(pad.region),
                        FactionTag(static_cast<FactionId>(pad.lastOwner < 0 ? 0 : pad.lastOwner)), pad.console,
                        pad.clutter);
#endif
    }

} // namespace Terrafront
