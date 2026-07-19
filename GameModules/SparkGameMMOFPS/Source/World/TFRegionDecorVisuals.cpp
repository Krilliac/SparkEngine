/**
 * @file TFRegionDecorVisuals.cpp
 * @brief TFRegionDecor presentation passes: static collision registration
 *        (RegisterCollision, both roles), the visual entity stamp
 *        (SpawnVisuals, HasLocalPlayer roles) and W10 distance culling
 *        (UpdateCulling). Split from TFRegionDecor.cpp per the repo file-size
 *        rules (same class — mirrors the TFWorldSetup/-Draw/-Render split);
 *        layout lives in TFRegionDecorLayout.cpp, instanced rendering in
 *        TFRegionDecorInstancing.cpp.
 */
#include "World/TFRegionDecor.h"

#include "Game/TFVisualUtils.h"     // FactionStructureMaterial (neutral/faction tint)
#include "World/TFDecorCulling.h"   // W10 distance-culling lane: ranges + hysteresis helper
#include "World/TFWeatherFx.h"      // W12 weather-visuals: storm cull-range scale (fog-in)
#include "World/TFWorldCollision.h" // AddModelObb / AddObbPart / OptimizeBroadPhase
#include "World/TFWorldSetup.h"     // Collision() + GetCamera() accessors

#include "Camera/SparkEngineCamera.h" // W10 distance culling: camera position
#include "Engine/ECS/Components.h"    // Transform, MeshRenderer
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <memory>
#include <string>

namespace Terrafront
{

    namespace
    {

        /// W10 distance-culling lane: cull class from the asset convention —
        /// scatter clutter lives under Models/MMOFPS/props/, kit pieces under
        /// buildings/. Anything unrecognized defaults to the building range
        /// (larger = safer visually; worst case it just culls later).
        float DecorCullRangeForModel(const std::string& model)
        {
            return model.find("/props/") != std::string::npos ? kDecorCullRangePropM : kDecorCullRangeBuildingM;
        }

    } // namespace

    // ---------------------------------------------------------------------------
    // Collision pass (both roles)
    // ---------------------------------------------------------------------------

    void TFRegionDecor::RegisterCollision()
    {
        // The scene TFWorldCollision was built inside TFWorldSetup::Initialize —
        // strictly before the first TFRegionSystem::Update (Main.cpp boot order)
        // — so registering here is always AFTER the .scene static bodies.
        TFWorldCollision* col = m_ctx->world ? m_ctx->world->Collision() : nullptr;
        if (!col)
        {
            m_collisionDone = true; // no collision owner shipped: decor stays walk-through
            return;
        }

        uint32_t added = 0;
        for (const LayoutPiece& p : m_layout)
        {
            if (!p.collide)
                continue;
            const float pos[3] = {p.x, p.y, p.z};

            // W11 gate-passages: authored parts REPLACE the whole-model OBB —
            // one rotated box per part (pillars/legs/lintels), leaving the
            // archway/underside genuinely walkable. Parts inherit the piece's
            // determinism (pure data + the bit-identical layout) and its
            // clearance demotion (p.collide was cleared above if any part
            // footprint violated). m_colSkipped counts per BODY, so a partly
            // failed piece still reports every miss.
            if (!p.collideParts.empty())
            {
                for (size_t k = 0; k < p.collideParts.size(); ++k)
                {
                    const DecorCollidePart& part = p.collideParts[k];
                    const std::string name = "TF_DecorCol_r" + std::to_string(p.region) + "_" +
                                             std::to_string(static_cast<unsigned>(m_colBodies.size())) + "_p" +
                                             std::to_string(static_cast<unsigned>(k));
                    std::shared_ptr<::PhysicsBody> body =
                        col->AddObbPart(pos, p.yawDeg, part.off, part.size, part.yawDeg, name);
                    if (body)
                    {
                        m_colBodies.push_back(std::move(body));
                        ++added;
                    }
                    else
                    {
                        ++m_colSkipped; // no Jolt / body cap — logged inside
                    }
                }
                continue;
            }

            const std::string name = "TF_DecorCol_r" + std::to_string(p.region) + "_" +
                                     std::to_string(static_cast<unsigned>(m_colBodies.size()));
            std::shared_ptr<::PhysicsBody> body = col->AddModelObb(p.model, pos, p.yawDeg, name);
            if (body)
            {
                m_colBodies.push_back(std::move(body));
                ++added;
            }
            else
            {
                ++m_colSkipped; // no Jolt / unreadable OBJ / body cap — logged inside
            }
        }

        // ONE broadphase re-compact after ALL decor statics (the scene set was
        // compacted once in Build(); this is the second and last static bulk).
        if (added > 0)
            col->OptimizeBroadPhase();

        m_collisionDone = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] decor: %u static OBBs registered (%u skipped, %u demoted)",
                       added, m_colSkipped, m_colDemoted);
    }

    // ---------------------------------------------------------------------------
    // Visual pass (HasLocalPlayer roles only)
    // ---------------------------------------------------------------------------

    void TFRegionDecor::SpawnVisuals()
    {
        // Visuals additionally need the engine ECS world (poll until it exists;
        // headless roles never reach here — Update gates on HasLocalPlayer).
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return;

        for (const LayoutPiece& p : m_layout)
        {
            const auto ent = world->CreateEntity("TF_Decor");
            Transform& tr = world->AddComponent<Transform>(ent);
            tr.position = {p.x, p.y, p.z};
            tr.rotation.y = p.yawDeg; // Transform Euler is DEGREES (radians rule is PhysicsBody-only)
            MeshRenderer& mr = world->AddComponent<MeshRenderer>(ent);
            mr.meshPath = p.model;
            mr.materialPath = p.material.empty() ? FactionStructureMaterial(*m_ctx, p.tint) : p.material;
            mr.castShadows = p.castShadows;
            mr.emissive = p.emissive;
            m_entities.push_back(static_cast<uint32_t>(ent));
            // W10 distance-culling lane: record spawn-time XZ + cull class
            // (decor never moves, so the entry never needs updating).
            m_cull.push_back({static_cast<uint32_t>(ent), p.x, p.z, DecorCullRangeForModel(p.model), /*visible*/ true});
        }

        m_visualsDone = true;
        m_cullVisible = static_cast<uint32_t>(m_cull.size()); // until the first cull pass runs
        m_cullHidden = 0;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] decor: %zu visual entities stamped", m_entities.size());
    }

    // ---------------------------------------------------------------------------
    // Distance culling (W10 distance-culling lane — SEPARATE section by design;
    // see World/TFDecorCulling.h for ranges, hysteresis and the rationale)
    // ---------------------------------------------------------------------------

    void TFRegionDecor::UpdateCulling()
    {
        // Re-evaluate every kDecorCullIntervalFrames frames (~0.25 s @ 60 fps);
        // the first call after SpawnVisuals (counter == 0) runs immediately so
        // far decor never draws even one full-rate frame burst.
        if (m_cullFrameCounter++ % kDecorCullIntervalFrames != 0)
            return;

        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        SparkEngineCamera* cam = m_ctx->world ? m_ctx->world->GetCamera() : nullptr;
        if (!world || !cam)
            return;
        const auto camPos = cam->GetPosition(); // XZ only: decor culls by ground distance

        // W12 weather-visuals: dust storms pull the cull ranges in (1.0 clear
        // -> 0.4 at full intensity) — the basic path has no fog, so distant
        // decor "fogging out" IS the storm's visibility read. VISUAL ONLY,
        // exactly like the base cull: never touches layout or collision.
        const float stormScale = TFWeatherFx::Get().DecorCullScale();

        uint32_t visible = 0;
        uint32_t hidden = 0;
        auto& registry = world->GetRegistry();
        for (size_t i = 0; i < m_cull.size(); ++i)
        {
            TFDecorCullEntry& e = m_cull[i];
            const auto ent = static_cast<EntityID>(e.entity);
            if (e.entity == 0u || !registry.valid(ent))
                continue;
            const float dx = e.x - camPos.x;
            const float dz = e.z - camPos.z;
            const bool want = DecorShouldBeVisible(dx * dx + dz * dz, e.cullRange * stormScale, e.visible);
            if (want != e.visible)
            {
                // MeshRenderer.visible short-circuits TFWorldSetup's ECS draw
                // loop before any mesh load or draw call — this flag IS the cull.
                // W12 decor-instancing: entities consumed by an instance group
                // stay ECS-invisible permanently (RenderInstanced reads
                // e.visible directly when filling instance buffers), so only
                // ungrouped decor flips the MeshRenderer flag here.
                const bool grouped = i < m_grouped.size() && m_grouped[i] != 0u;
                if (!grouped)
                {
                    if (MeshRenderer* mr = registry.try_get<MeshRenderer>(ent))
                        mr->visible = want;
                }
                e.visible = want;
            }
            if (want)
                ++visible;
            else
                ++hidden;
        }
        m_cullVisible = visible;
        m_cullHidden = hidden;
    }

} // namespace Terrafront
