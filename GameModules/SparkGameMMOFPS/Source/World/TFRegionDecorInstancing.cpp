/**
 * @file TFRegionDecorInstancing.cpp
 * @brief TFRegionDecor W12 decor-instancing lane: BuildInstanceGroups,
 *        DissolveInstanceGroups and RenderInstanced (grouped decor draws
 *        through the engine's basic instanced pipeline — one
 *        GraphicsEngine::DrawMeshInstanced per group). Split from
 *        TFRegionDecor.cpp per the repo file-size rules (same class — mirrors
 *        the TFWorldSetup/-Draw/-Render split); see RenderInstanced in the
 *        header for the opt-in contract.
 */
#include "World/TFRegionDecor.h"

#include "Game/TFVisualUtils.h" // FactionStructureMaterial (neutral/faction tint)

#include "Engine/ECS/Components.h" // MeshRenderer
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Graphics/WorldBasicRenderer.h" // Spark::WorldMeshCache (tinyobjloader path)

#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Instanced rendering (W12 decor-instancing lane — SEPARATE section by
    // design; see RenderInstanced in the header for the opt-in contract)
    // ---------------------------------------------------------------------------

    namespace
    {
        /// Groups smaller than this stay on the per-entity ECS path — below
        /// four instances the extra instance-buffer Map/upload costs more than
        /// the draw calls it saves.
        constexpr uint32_t kMinInstancedGroup = 4;
    } // namespace

    void TFRegionDecor::BuildInstanceGroups(GraphicsEngine* gfx)
    {
        // One deterministic attempt per arm: whatever this pass decides
        // (groups, or per-entity fallback) stays until Shutdown or a
        // tf_decor_inst re-arm. Clear first so a re-arm never duplicates.
        m_groupsBuilt = true;
        m_groups.clear();

        // Feature probe: engines without the basic instanced pipeline (compile
        // failure, Linux) keep the per-entity path — zero behavior change.
        if (!gfx || !gfx->HasInstancedBasicPipeline())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game,
                           "[TF] decor: instanced pipeline unavailable — per-entity draw path kept");
            return;
        }

        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return;

        // SpawnVisuals pushes m_entities and m_cull in lock-step with its
        // m_layout iteration, so index i is the same piece in all three. If a
        // future restructure breaks that invariant, bail to per-entity rather
        // than group the wrong transforms under the wrong mesh.
        if (m_layout.size() != m_cull.size() || m_entities.size() != m_cull.size())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] decor: layout/visuals index mismatch (%zu/%zu/%zu) — instancing skipped",
                           m_layout.size(), m_entities.size(), m_cull.size());
            return;
        }

        m_grouped.assign(m_cull.size(), 0u);

        // Partition by (model, resolved material, emissive) — everything the
        // per-entity draw varies per decor piece. Decor entities never carry
        // TFFactionComp, so the ECS loop's faction tint is always white for
        // them; material resolution matches SpawnVisuals exactly (skyanchor
        // pieces resolve their home-faction structure material).
        std::unordered_map<std::string, size_t> keyToGroup;
        std::vector<DecorInstanceGroup> groups;
        char key[560];
        for (size_t i = 0; i < m_layout.size(); ++i)
        {
            const LayoutPiece& p = m_layout[i];
            const std::string& material = p.material.empty() ? FactionStructureMaterial(*m_ctx, p.tint) : p.material;
            std::snprintf(key, sizeof(key), "%s|%s|%.3f", p.model.c_str(), material.c_str(),
                          static_cast<double>(p.emissive));

            const auto [it, inserted] = keyToGroup.try_emplace(key, groups.size());
            if (inserted)
            {
                DecorInstanceGroup g;
                g.model = p.model;
                g.material = material;
                g.emissive = p.emissive;
                groups.push_back(std::move(g));
            }
            DecorInstanceGroup& g = groups[it->second];
            g.cullIdx.push_back(static_cast<uint32_t>(i));

            // EXACTLY Transform::GetLocalMatrix for this entity (scale 1,
            // yaw-only Euler in DEGREES, no parent): the same
            // XMMatrixRotationRollPitchYaw + XMMatrixTranslation calls, so the
            // instanced world matrix is bit-identical to the per-entity one.
            using namespace DirectX;
            const XMMATRIX wm = XMMatrixRotationRollPitchYaw(0.0f, XMConvertToRadians(p.yawDeg), 0.0f) *
                                XMMatrixTranslation(p.x, p.y, p.z);
            XMFLOAT4X4 w4;
            XMStoreFloat4x4(&w4, wm);
            g.worlds.push_back(w4);
        }

        // Keep only groups worth instancing; hide their entities from the
        // per-entity ECS loop (the instanced path draws them from now on).
        auto& registry = world->GetRegistry();
        uint32_t groupedPieces = 0;
        for (auto& g : groups)
        {
            if (g.cullIdx.size() < kMinInstancedGroup)
                continue; // stays per-entity (MeshRenderer.visible untouched)
            for (const uint32_t idx : g.cullIdx)
            {
                m_grouped[idx] = 1u;
                const auto ent = static_cast<EntityID>(m_cull[idx].entity);
                if (m_cull[idx].entity != 0u && registry.valid(ent))
                {
                    if (MeshRenderer* mr = registry.try_get<MeshRenderer>(ent))
                        mr->visible = false;
                }
            }
            groupedPieces += static_cast<uint32_t>(g.cullIdx.size());
            m_groups.push_back(std::move(g));
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] decor: instancing %u pieces in %zu groups (%zu pieces stay per-entity)", groupedPieces,
                       m_groups.size(), m_layout.size() - groupedPieces);
    }

    void TFRegionDecor::DissolveInstanceGroups()
    {
        // Un-opt-in (runtime draw failure, or the tf_decor_inst A/B toggle):
        // hand every grouped entity back to the per-entity ECS loop with its
        // current cull state, then drop the groups (m_groupsBuilt stays set —
        // no rebuild unless tf_decor_inst re-arms it).
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (world)
        {
            auto& registry = world->GetRegistry();
            for (const DecorInstanceGroup& g : m_groups)
            {
                for (const uint32_t idx : g.cullIdx)
                {
                    const auto ent = static_cast<EntityID>(m_cull[idx].entity);
                    if (m_cull[idx].entity != 0u && registry.valid(ent))
                    {
                        if (MeshRenderer* mr = registry.try_get<MeshRenderer>(ent))
                            mr->visible = m_cull[idx].visible;
                    }
                }
            }
        }
        m_groups.clear();
        m_grouped.clear();
        m_instDrawsLast = 0;
        m_instInstancesLast = 0;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] decor: instance groups dissolved — per-entity path active");
    }

    void TFRegionDecor::RenderInstanced(GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                        const DirectX::XMMATRIX& proj)
    {
        if (!m_initialized || !m_visualsDone || !gfx)
            return;
        if (!m_groupsBuilt)
            BuildInstanceGroups(gfx);
        if (m_groups.empty())
            return;

        if (!m_meshCache)
            m_meshCache = std::make_unique<Spark::WorldMeshCache>();

        m_instDrawsLast = 0;
        m_instInstancesLast = 0;

        // Instanced VS + the SHARED basic PS; b1 (view*proj, lighting) was set
        // by RenderWorld's UpdateFrameConstants for this exact view/proj.
        gfx->SetBasicShadersInstanced();

        bool drawFailed = false;
        for (const DecorInstanceGroup& g : m_groups)
        {
            // Visible subset (spawn-order stable): the cull pass maintains
            // e.visible for grouped entries without touching MeshRenderer.
            m_instScratch.clear();
            for (size_t k = 0; k < g.cullIdx.size(); ++k)
            {
                if (m_cull[g.cullIdx[k]].visible)
                    m_instScratch.push_back(g.worlds[k]);
            }
            if (m_instScratch.empty())
                continue;

            Mesh* mesh = m_meshCache->GetOrLoad(*gfx, g.model);
            if (!mesh || mesh->GetIndexCount() == 0)
                continue;

            // Material resolution mirrors the per-entity ECS loop in
            // TFWorldSetup::RenderWorld (decor materials are always JSON).
            const GraphicsEngine::BasicMaterial* mat =
                g.material.empty() ? nullptr : gfx->GetOrLoadBasicMaterial(g.material);
            ID3D11ShaderResourceView* matSrv = mat ? mat->srv.Get() : nullptr;
            const DirectX::XMFLOAT2 matTiling = mat ? mat->tiling : DirectX::XMFLOAT2{1.0f, 1.0f};
            gfx->SetBasicMaterialTextures(mat ? mat->normalSrv.Get() : nullptr,
                                          mat ? mat->roughnessSrv.Get() : nullptr);

            const uint32_t count = static_cast<uint32_t>(m_instScratch.size());
            const DirectX::XMFLOAT4X4* worlds = m_instScratch.data();
            const DirectX::XMMATRIX identity = DirectX::XMMatrixIdentity();
            const auto& submeshes = mesh->GetSubmeshes();
            if (submeshes.empty())
            {
                // b0 world part is ignored by the instanced VS (identity is
                // just a placeholder); color/tiling/emissive ride b0 per group
                // exactly as the per-entity path sets them per entity.
                gfx->UpdateBasicConstants(identity, view, proj, DirectX::XMFLOAT4{1.0f, 1.0f, 1.0f, 1.0f}, matTiling,
                                          g.emissive);
                gfx->SetBasicTexture(matSrv);
                if (!gfx->DrawMeshInstanced(*mesh, worlds, count))
                {
                    drawFailed = true;
                    break;
                }
                ++m_instDrawsLast;
                m_instInstancesLast += count;
            }
            else
            {
                // OBJ submesh ranges: same texture/tint fallback rules as the
                // per-entity loop (map_Kd at 1:1 UVs, else material texture
                // tinted by the MTL Kd), one instanced draw per range.
                for (const MeshSubmesh& smesh : submeshes)
                {
                    ID3D11ShaderResourceView* srv =
                        smesh.diffuseTexture.empty() ? nullptr : gfx->GetOrLoadTextureSRV(smesh.diffuseTexture);
                    DirectX::XMFLOAT2 tiling{1.0f, 1.0f};
                    if (!srv)
                    {
                        srv = matSrv;
                        tiling = matTiling;
                    }
                    gfx->UpdateBasicConstants(identity, view, proj, smesh.diffuseColor, tiling, g.emissive);
                    gfx->SetBasicTexture(srv);
                    if (!gfx->DrawMeshInstanced(*mesh, worlds, count, smesh.indexStart, smesh.indexCount))
                    {
                        drawFailed = true;
                        break;
                    }
                    ++m_instDrawsLast;
                    m_instInstancesLast += count;
                }
                if (drawFailed)
                    break;
            }
        }

        // Hand the pipeline back to the per-entity basic path (viewmodel, FX
        // and later passes rebind SetBasicShaders anyway — this keeps the
        // frame's state invariants explicit).
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(nullptr);
        gfx->SetBasicMaterialTextures(nullptr, nullptr);

        if (drawFailed)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] decor: DrawMeshInstanced failed — falling back to the per-entity path");
            DissolveInstanceGroups();
        }
    }

} // namespace Terrafront
