/**
 * @file TFWorldSetupRender.cpp
 * @brief TFWorldSetup frame composition (RenderWorld): the module-owned
 *        BeginFrame/EndFrame pair — skybox, terrain, scene geometry, ECS
 *        visuals, viewmodel, shot/ground/impact/weather FX and the
 *        transparent pass. Draw helpers live in TFWorldSetupDraw.cpp;
 *        scene/terrain load lives in TFWorldSetup.cpp (same class, split
 *        per the repo file-size rules — mirrors the TFRegionSystem/-Net split).
 */
#include "World/TFWorldSetup.h"

#include "Data/TFDataTables.h"
#include "Game/TFWeaponSystem.h"

#include "Spark/IEngineContext.h"
#include "SceneManager/SceneManager.h"
#include "Engine/ECS/Components.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Game/TFComponents.h"
#include "Game/TFGroundFx.h"
#include "Game/TFBlobShadows.h"   // W13 shadow-polish lane: ground-projected blob shadows
#include "Game/TFVehicleFx.h"     // W13 vehicle-damage-states lane: hull-hp smoke/spark/fire
#include "World/TFWeatherFx.h"    // W12 weather-visuals: storm cycle + client visuals
#include "World/TFRegionDecor.h"  // W12 decor-instancing: RenderInstanced opt-in
#include "World/TFRegionSystem.h" // W12 decor-instancing: Decor() accessor
#include "Game/TFImpactFx.h"      // impact-fx lane (W10)
#include "Game/TFGrenadeSystem.h" // grenades lane (W10): replicated grenade fx
#include "Game/TFPingSystem.h"    // ping-system lane (W11): world-space ping diamonds
#include "Game/TFViewModel.h"
#include "Game/TFTransparentPass.h"
#include "Utils/LogMacros.h"

#include <cmath>
#include <exception>

namespace Terrafront
{

    void TFWorldSetup::RenderWorld()
    {
        if (!m_initialized || !m_ctx || !m_ctx->engine)
            return;
        GraphicsEngine* gfx = m_ctx->engine->GetGraphics();
        if (!gfx)
            return;

        // Mirrors GameModules/SparkGameFPS Game::Render: the ONLY
        // BeginFrame/EndFrame pair per frame while this module is loaded.
        gfx->BeginFrame();
        try
        {
            DirectX::XMMATRIX view, proj;
            ComputeViewProj(view, proj);

            // Per-frame constants (b1): the basic pixel shader reads the
            // directional/ambient light from this buffer — without this call it
            // stays unwritten and everything shades to black.
            {
                const DirectX::XMMATRIX invView = DirectX::XMMatrixInverse(nullptr, view);
                DirectX::XMFLOAT3 camPos;
                DirectX::XMStoreFloat3(&camPos, invView.r[3]);
                gfx->UpdateFrameConstants(view, proj, camPos);
            }

            // 0) Skybox first, so the world's opaque geometry overdraws it.
            DrawSkybox(view, proj);

            // 0b) Procedural terrain relief (replaces the flat scene ground plane).
            DrawTerrain(view, proj);

            // 1) Scene geometry (terrain plane, mesas, buildings) — the continent
            //    scene plus the additive Sanctuary Haven zone (continents lane).
            //    Draws are issued through GraphicsEngine/Mesh MEMBER functions:
            //    GameObject::Render() reads EngineContext::Get(), which is a
            //    per-image global and unset inside this statically-linked DLL.
            DrawSceneObjects(m_scene, view, proj);
            DrawSceneObjects(m_sanctuaryScene.get(), view, proj);

            // 2) ECS visuals — pawns, vehicles and deployables all attach a
            //    MeshRenderer (TFPlayerSystem::AttachPawnVisual & friends). The
            //    engine's SubmitMeshForRendering/ProcessDrawList path loads meshes
            //    through the AssetPipeline (OBJ loader unreliable on Windows), so
            //    pawns never drew. Draw them through the SAME device-direct mesh +
            //    basic-shader path the scene geometry above uses.
            if (World* world = m_ctx->engine->GetWorld())
            {
                ID3D11DeviceContext* dc = gfx->GetContext();
                gfx->SetBasicShaders();
                const auto& registry = world->GetRegistry();
                auto ecsView = world->GetEntitiesWith<Transform, MeshRenderer>();
                for (auto entity : ecsView)
                {
                    const MeshRenderer& mr = ecsView.get<MeshRenderer>(entity);
                    if (!mr.visible)
                        continue;
                    if (const auto* active = registry.try_get<ActiveComponent>(entity); active && !active->active)
                        continue;

                    Mesh* mesh = GetOrLoadEcsMesh(mr.meshPath);
                    if (!mesh || mesh->GetVertexCount() == 0 || mesh->GetIndexCount() == 0)
                        continue;

                    const GraphicsEngine::BasicMaterial* mat =
                        mr.materialPath.empty() ? nullptr : gfx->GetOrLoadBasicMaterial(mr.materialPath);
                    ID3D11ShaderResourceView* matSrv = mat ? mat->srv.Get() : nullptr;
                    const DirectX::XMFLOAT2 matTiling = mat ? mat->tiling : DirectX::XMFLOAT2{1.0f, 1.0f};
                    // W8 render-pbr-lite: per-material normal/roughness (defaults on nullptr).
                    gfx->SetBasicMaterialTextures(mat ? mat->normalSrv.Get() : nullptr,
                                                  mat ? mat->roughnessSrv.Get() : nullptr);

                    const DirectX::XMMATRIX worldM = ecsView.get<Transform>(entity).GetWorldMatrix(registry);
                    const auto& submeshes = mesh->GetSubmeshes();

                    // Faction tint: multiply the model's own texture/color by a
                    // moderated faction hue so friend/foe read at a glance across
                    // the battlefield. lerp(white, factionColor, 0.55) keeps the
                    // colormap legible while clearly coloring the body.
                    DirectX::XMFLOAT4 tint{1.0f, 1.0f, 1.0f, 1.0f};
                    if (const auto* fc = registry.try_get<TFFactionComp>(entity); fc && fc->faction != FactionId::None)
                    {
                        float fcol[4];
                        FactionColor(fc->faction, fcol);
                        constexpr float k = 0.40f; // moderate tint; keep the body bright/legible
                        tint.x = 1.0f + k * (fcol[0] - 1.0f);
                        tint.y = 1.0f + k * (fcol[1] - 1.0f);
                        tint.z = 1.0f + k * (fcol[2] - 1.0f);
                    }

                    const float emissive = mr.emissive; // 0 == unchanged (plain path)
                    if (submeshes.empty())
                    {
                        gfx->UpdateBasicConstants(worldM, view, proj, tint, matTiling, emissive);
                        gfx->SetBasicTexture(matSrv);
                        mesh->Render(dc);
                    }
                    else
                    {
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
                            const DirectX::XMFLOAT4 c{smesh.diffuseColor.x * tint.x, smesh.diffuseColor.y * tint.y,
                                                      smesh.diffuseColor.z * tint.z, smesh.diffuseColor.w};
                            gfx->UpdateBasicConstants(worldM, view, proj, c, tiling, emissive);
                            gfx->SetBasicTexture(srv);
                            mesh->RenderRange(dc, smesh.indexStart, smesh.indexCount);
                        }
                    }
                }
                gfx->SetBasicTexture(nullptr);
                gfx->SetBasicMaterialTextures(nullptr, nullptr); // W8 pbr-lite: flat defaults
            }

            // 2b) W12 decor-instancing: grouped region decor — one
            //     DrawMeshInstanced per (mesh, material, emissive) group.
            //     Small groups (< 4) and everything else stayed in the
            //     per-entity loop above; falls back automatically when the
            //     engine lacks the instanced pipeline. Uses this frame's
            //     view/proj and the b1 constants set at the top of RenderWorld.
            if (m_ctx->regions)
            {
                if (TFRegionDecor* decor = m_ctx->regions->Decor())
                    decor->RenderInstanced(gfx, view, proj);
            }

            // 3) First-person viewmodel — arms + equipped weapon with sway/walk
            //    bob/recoil (Game/TFViewModel.h; supersedes the old static draw —
            //    keeping both double-draws the gun).
            if (m_ctx->HasLocalPlayer() && m_ctx->weapons)
                TFViewModel::Get().Render(*m_ctx, gfx, view, proj);

            // 4) Shot effects: bright unlit muzzle flash + tracer per recent shot.
            //    Drawn as stretched unit cubes; the over-1.0 color clamps bright
            //    even after the basic shader's lighting multiply.
            if (!m_shotFx.empty())
            {
                if (!m_fxCube)
                {
                    m_fxCube = std::make_unique<Mesh>();
                    m_fxCube->Initialize(gfx->GetDevice(), gfx->GetContext());
                    m_fxCube->CreateCube(1.0f);
                }
                if (m_fxCube && m_fxCube->GetIndexCount() > 0)
                {
                    ID3D11DeviceContext* dc = gfx->GetContext();
                    gfx->SetBasicShaders();
                    gfx->SetBasicTexture(nullptr);
                    using namespace DirectX;
                    const MuzzleFxDef& fxDef = Pres().muzzleFx;
                    for (const ShotFx& fx : m_shotFx)
                    {
                        const double age = m_fxClock - fx.t0;
                        XMVECTOR o = XMLoadFloat3(&fx.origin);
                        XMVECTOR f = XMVector3Normalize(XMLoadFloat3(&fx.dir));
                        XMVECTOR upRef =
                            (std::fabs(XMVectorGetY(f)) > 0.99f) ? XMVectorSet(1, 0, 0, 0) : XMVectorSet(0, 1, 0, 0);
                        XMVECTOR r = XMVector3Normalize(XMVector3Cross(upRef, f));
                        XMVECTOR u = XMVector3Cross(f, r);
                        XMMATRIX orient = XMMatrixIdentity();
                        orient.r[0] = r;
                        orient.r[1] = u;
                        orient.r[2] = f;

                        // Muzzle is at the gun tip (lower-right of view, ~1.2 m
                        // forward) so effects clear the 0.5 m near plane and line up
                        // with the first-person weapon instead of the eye.
                        XMVECTOR muzzle = o + f * fxDef.muzzleForwardM + r * fxDef.muzzleRightM + u * fxDef.muzzleUpM;

                        // Tracer: thin bright streak from the muzzle forward. Edge-on
                        // from the shooter, but a clear streak from any side angle.
                        if (age < fxDef.tracerLifeSec)
                        {
                            const float kLen = fxDef.tracerLenM, kThick = fxDef.tracerThickM;
                            XMVECTOR mid = XMVectorAdd(muzzle, XMVectorScale(f, kLen * 0.5f));
                            XMMATRIX w =
                                XMMatrixScaling(kThick, kThick, kLen) * orient * XMMatrixTranslationFromVector(mid);
                            gfx->UpdateBasicConstants(w, view, proj,
                                                      XMFLOAT4(fxDef.tracerColor[0], fxDef.tracerColor[1],
                                                               fxDef.tracerColor[2], fxDef.tracerColor[3]),
                                                      XMFLOAT2(1, 1));
                            m_fxCube->Render(dc);
                        }
                        // Muzzle flash: bright puff at the gun tip (first ~50 ms).
                        if (age < fxDef.flashLifeSec)
                        {
                            XMMATRIX w =
                                XMMatrixScaling(fxDef.flashScale[0], fxDef.flashScale[1], fxDef.flashScale[2]) *
                                orient * XMMatrixTranslationFromVector(muzzle);
                            gfx->UpdateBasicConstants(w, view, proj,
                                                      XMFLOAT4(fxDef.flashColor[0], fxDef.flashColor[1],
                                                               fxDef.flashColor[2], fxDef.flashColor[3]),
                                                      XMFLOAT2(1, 1));
                            m_fxCube->Render(dc);
                        }
                    }
                    gfx->SetBasicTexture(nullptr);
                }
            }

            // 4b) Blob shadows (W13 shadow-polish lane): soft dark ground discs
            //     under live pawns/vehicles/deployables — the basic path's scoped
            //     stand-in for a real shadow map (Game/TFBlobShadows.h; cap 128,
            //     camera-distance culled, height-above-ground faded). Drawn before
            //     the additive ground/impact puffs so the shadow reads under them.
            if (m_ctx->HasLocalPlayer())
                TFBlobShadows::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5) Hover dust: client-side flipbook ground puffs under fast-moving
            //    vehicles (Game/TFGroundFx.h; camera-facing additive quads, cap 64).
            if (m_ctx->HasLocalPlayer())
                TFGroundFx::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5b) Bullet-impact bursts: pooled flipbook quads at local/remote
            //     shot impact points (Game/TFImpactFx.h; cap 48, ~0.15 s).
            if (m_ctx->HasLocalPlayer())
                TFImpactFx::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5b2) Vehicle damage FX (W13 vehicle-damage-states lane): hull-hp
            //      smoke/spark/fire billboards + a critical-tier pitched-down
            //      engine cue on damaged hulls (Game/TFVehicleFx.h; reads
            //      TFVehicleInfo.hp via the existing ForEachVehicle enumerator).
            if (m_ctx->HasLocalPlayer())
                TFVehicleFx::Get().UpdateAndRender(*m_ctx, gfx, view, proj);

            // 5c) Grenades (W10): replicated grenade bodies + boom flipbook
            //     (Game/TFGrenadeSystem.h; restores blend/depth/texture state).
            if (m_ctx->HasLocalPlayer() && m_ctx->grenades)
                m_ctx->grenades->RenderClientFx(gfx, view, proj);

            // 5d) Pings (W11): squad-scoped tactical ping diamonds
            //     (Game/TFPingSystem.h; restores blend/depth/texture state).
            if (m_ctx->HasLocalPlayer() && m_ctx->pings)
                m_ctx->pings->RenderClientFx(gfx, view, proj);

            // 6) Transparent surfaces (shield-wall energy planes + any queued
            //    alpha FX) — sorted back-to-front and drawn AFTER all opaque
            //    passes (W8 render-transparency lane; producers queue during
            //    their Update, e.g. TFDeployableSystem's shield walls).
            TFTransparentPass::Get().Flush(gfx, view, proj);

            // 7) Dust storm (W12): wind-blown dust billboards + fullscreen sandy
            //    tint, drawn last so the wash covers transparents too
            //    (World/TFWeatherFx.h; cap 120, restores blend/depth/texture).
            if (m_ctx->HasLocalPlayer())
                TFWeatherFx::Get().Render(*m_ctx, gfx, view, proj);
        }
        catch (const std::exception& e)
        {
            SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "TFRender", 5, "[TF] RenderWorld exception: %s", e.what());
        }
        catch (...)
        {
            SPARK_LOG_EVERY_SECONDS(Spark::LogLevel::Error, "TFRender", 5, "[TF] RenderWorld: unknown exception");
        }
        gfx->EndFrame();
    }

} // namespace Terrafront
