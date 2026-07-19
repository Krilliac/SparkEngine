/**
 * @file TFRegionDecor.cpp
 * @brief TFRegionDecor lifecycle: construction, Initialize (console commands),
 *        the per-frame Update gate and Shutdown teardown. Split per the repo
 *        file-size rules (same class — mirrors the TFWorldSetup/-Draw/-Render
 *        split): decor.json parsing lives in TFRegionDecorData.cpp, the
 *        role-agnostic layout in TFRegionDecorLayout.cpp, collision/visuals/
 *        culling in TFRegionDecorVisuals.cpp and the W12 instanced-draw lane
 *        in TFRegionDecorInstancing.cpp (see TFRegionDecor.h for the W10
 *        split and the determinism contract).
 */
#include "World/TFRegionDecor.h"

#include "World/TFWorldCollision.h" // RemoveBody (Shutdown)
#include "World/TFWorldSetup.h"     // Collision() accessor

#include "Engine/ECS/Components.h" // World (registry access in Shutdown)
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

// Complete Spark::WorldMeshCache: Shutdown (and the ~TFRegionDecor it runs
// from) resets the m_meshCache unique_ptr, which needs the full type here.
#include "Graphics/WorldBasicRenderer.h"

#include <cstdio>

namespace Terrafront
{

    TFRegionDecor::TFRegionDecor() = default;
    TFRegionDecor::~TFRegionDecor()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFRegionDecor::Initialize(TFGameContext& ctx)
    {
        m_ctx = &ctx;
        m_initialized = true;

        auto& console = Spark::SimpleConsole::GetInstance();
        if (!console.HasCommand("tf_decor_debug"))
        {
            console.RegisterCommand(
                "tf_decor_debug",
                [this](const std::vector<std::string>&) -> std::string
                {
                    char buf[448];
                    std::snprintf(buf, sizeof(buf),
                                  "[TF] decor: layout=%d pieces=%zu visuals=%u colBodies=%u colSkipped=%u "
                                  "colDemoted=%u skipped(clearance)=%u skipped(budget)=%u templates=%zu "
                                  "cullVisible=%u cullHidden=%u instGroups=%zu instDraws=%u instInstances=%u",
                                  m_layoutDone ? 1 : 0, m_layout.size(), SpawnedCount(), CollisionBodyCount(),
                                  m_colSkipped, m_colDemoted, m_skippedClearance, m_skippedBudget, m_templates.size(),
                                  VisibleDecorCount(), CulledDecorCount(), m_groups.size(), m_instDrawsLast,
                                  m_instInstancesLast);
                    return std::string(buf);
                },
                "Region decor status: layout pieces, visual entities, collision bodies, clearance/budget skips, "
                "distance-cull visible/hidden counts, instanced-draw groups/calls",
                "TERRAFRONT", "tf_decor_debug");
            m_debugCmd = true;
        }

        // W12 decor-instancing lane: A/B toggle — the visual sanity gate is
        // "tf_decor_inst off" vs "on" looking pixel-identical (same meshes,
        // materials, world matrices and lighting through both paths).
        if (!console.HasCommand("tf_decor_inst"))
        {
            console.RegisterCommand(
                "tf_decor_inst",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!args.empty() && (args[0] == "off" || args[0] == "0"))
                    {
                        DissolveInstanceGroups();
                        return "[TF] decor instancing OFF (per-entity path restored)";
                    }
                    if (!args.empty() && (args[0] == "on" || args[0] == "1"))
                    {
                        m_groupsBuilt = false; // re-partition on the next RenderInstanced
                        return "[TF] decor instancing re-armed (groups rebuild next frame)";
                    }
                    char buf[96];
                    std::snprintf(buf, sizeof(buf), "[TF] decor instancing: %s (%zu groups) — tf_decor_inst on|off",
                                  m_groups.empty() ? "inactive" : "active", m_groups.size());
                    return std::string(buf);
                },
                "A/B toggle for instanced decor rendering (visual sanity: on and off must look identical)",
                "TERRAFRONT", "tf_decor_inst");
            m_instCmd = true;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFRegionDecor initialized");
        return true;
    }

    void TFRegionDecor::Update()
    {
        if (!m_initialized || !m_ctx)
            return;
        if (!m_layoutDone && !TryComputeLayout())
            return; // prerequisites not live yet — poll again next frame
        if (!m_collisionDone)
            RegisterCollision();
        if (!m_visualsDone && m_ctx->HasLocalPlayer())
            SpawnVisuals();
        if (m_visualsDone && !m_cull.empty())
            UpdateCulling(); // W10 distance-culling lane: ~4 Hz visibility pass
    }

    void TFRegionDecor::Shutdown()
    {
        if (!m_initialized)
            return;

        if (m_debugCmd)
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_decor_debug");
            m_debugCmd = false;
        }
        if (m_instCmd) // W12 decor-instancing lane
        {
            Spark::SimpleConsole::GetInstance().UnregisterCommand("tf_decor_inst");
            m_instCmd = false;
        }

        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (world)
        {
            for (const uint32_t ent : m_entities)
            {
                const auto e = static_cast<EntityID>(ent);
                if (ent != 0u && world->GetRegistry().valid(e))
                    world->DestroyEntity(e);
            }
        }
        m_entities.clear();
        m_cull.clear(); // W10 distance-culling lane
        m_cullFrameCounter = 0;
        m_cullVisible = 0;
        m_cullHidden = 0;

        // W12 decor-instancing lane: groups reference m_cull indexes and the
        // entities destroyed above — drop everything (the mesh cache releases
        // its device buffers here, safely before engine teardown: Main.cpp
        // shuts TFRegionSystem down while the GraphicsEngine is still alive).
        m_groups.clear();
        m_grouped.clear();
        m_instScratch.clear();
        m_meshCache.reset();
        m_groupsBuilt = false;
        m_instDrawsLast = 0;
        m_instInstancesLast = 0;

        // Collision handles: Main.cpp shuts TFRegionSystem (and therefore us)
        // down BEFORE TFWorldSetup, so the scene collision set is still alive
        // here; if it is already gone, RemoveBody on a foreign handle is a
        // safe no-op and the set's own Shutdown removed the bodies anyway.
        TFWorldCollision* col = (m_ctx && m_ctx->world) ? m_ctx->world->Collision() : nullptr;
        if (col)
        {
            for (const auto& body : m_colBodies)
                col->RemoveBody(body);
        }
        m_colBodies.clear();

        m_layout.clear();
        m_templates.clear();
        m_layoutDone = false;
        m_collisionDone = false;
        m_visualsDone = false;
        m_loadTried = false;
        m_loaded = false;
        m_initialized = false;
    }

} // namespace Terrafront
