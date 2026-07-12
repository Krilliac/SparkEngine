/**
 * @file TFBlobShadows.cpp
 * @brief Ground-projected blob shadow renderer (see TFBlobShadows.h for the
 *        wiring contract and the design rationale).
 */
#include "Game/TFBlobShadows.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFDeployableTypes.h"
#include "World/TFWorldSetup.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include "Engine/World/TimeOfDaySystem.h"

#include <cmath>

namespace Terrafront
{

    namespace
    {

        // Pawns are a fixed capsule footprint; vehicles vary by kind (below);
        // deployables use TFDeploySpecOf()->overlapRadiusM with this as the
        // fallback for an unrecognized kind.
        constexpr float kPawnBlobRadiusM = 0.5f;
        constexpr float kDeployableFallbackRadiusM = 0.9f;

    } // namespace

    TFBlobShadows& TFBlobShadows::Get()
    {
        static TFBlobShadows s_instance;
        return s_instance;
    }

    float TFBlobShadows::VehicleBlobRadiusM(VehicleId id)
    {
        // Local approximation, not authoritative geometry -- every FX lane in
        // this module keeps its own small table (TFGroundFx.cpp / TFImpactFx.cpp
        // HullRadiusM are the sibling precedents); slightly larger than those
        // since a shadow footprint should read a touch bigger than the hull.
        switch (id)
        {
        case VehicleId::Drifter:
            return 1.3f;
        case VehicleId::Aegis:
            return 2.6f;
        case VehicleId::Ravager:
            return 2.4f;
        case VehicleId::Vulture:
            return 2.2f;
        default:
            return 1.8f;
        }
    }

    void TFBlobShadows::Consider(TFGameContext& ctx, const float pos[3], float radiusM, const float camPos[3],
                                 float sunOffX, float sunOffZ)
    {
        if (!ctx.world || radiusM <= 0.0f)
            return;
        if (m_scratch.size() >= kMaxBlobs)
            return;

        const float dx = pos[0] - camPos[0];
        const float dz = pos[2] - camPos[2];
        const float distSq = dx * dx + dz * dz;
        if (distSq > kMaxDrawDistM * kMaxDrawDistM)
            return;

        // Terrain sample at the entity's OWN (x,z) -- the small sun offset is
        // applied to the draw position only, not the height query, which is
        // an acceptable approximation at this offset magnitude (<= kMaxSunOffsetM)
        // and keeps this to one TerrainHeightAt call per candidate.
        const float terrainY = ctx.world->TerrainHeightAt(pos[0], pos[2]);
        float heightAbove = pos[1] - terrainY;
        if (heightAbove < 0.0f)
            heightAbove = 0.0f; // slopes/ramps can sink the pivot a hair below the sample; never a negative fade

        // Explicit clamp (windows.h min/max macros make std::clamp hazardous
        // in this codebase -- GraphicsDeviceResourcesWindows.cpp precedent).
        float fadeT = (heightAbove - kFadeStartM) / (kFadeEndM - kFadeStartM);
        if (fadeT < 0.0f)
            fadeT = 0.0f;
        else if (fadeT > 1.0f)
            fadeT = 1.0f;

        const float alpha = kBaseAlpha * (1.0f - fadeT);
        if (alpha < 0.02f)
            return; // fully faded (airborne) -- not worth a draw call

        BlobDraw draw;
        draw.x = pos[0] + sunOffX;
        draw.z = pos[2] + sunOffZ;
        draw.y = terrainY + 0.03f; // lift off the terrain to avoid z-fighting
        // CreatePlane(1,1) is already a 1x1 unit square (corners at +-0.5), so
        // a world-scale of S produces a final edge length of S meters; double
        // radiusM so the soft circle baked into the texture (inscribed in
        // that square) reads as a disc of radius ~radiusM, not radiusM/2.
        draw.scale = 2.0f * radiusM * (1.0f - 0.25f * fadeT); // shrinks a little as it rises, on top of fading
        draw.alpha = alpha;
        m_scratch.push_back(draw);
    }

    void TFBlobShadows::UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                        const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!gfx || !gfx->GetDevice() || !gfx->GetContext() || !ctx.world)
            return;

        m_scratch.clear();

        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        XMFLOAT3 camPosF;
        XMStoreFloat3(&camPosF, invView.r[3]);
        const float camPos[3] = {camPosF.x, camPosF.y, camPosF.z};

        // Sun-direction ground offset: a cheap directional feel, NOT a true
        // shadow projection. Spark::TimeOfDaySystem::GetInstance() is a
        // per-module-image static (see TFDayNight.h's per-image-DLL-statics
        // note) -- TFDayNight lives in this SAME module DLL and drives this
        // exact instance every Update(), so reading it here needs no
        // GraphicsEngine/cross-DLL plumbing. If TFDayNight was never wired
        // (or this is a build without it) the instance defaults to straight
        // overhead noon (sun = (0,1,0)), which makes sunOffX/sunOffZ == 0 --
        // blobs simply sit centered, exactly like before this offset existed.
        float sunOffX = 0.0f, sunOffZ = 0.0f;
        {
            const XMFLOAT3 sun = Spark::TimeOfDaySystem::GetInstance().GetSunDirection(); // unit vector TOWARD the sun
            const float horiz = sqrtf(sun.x * sun.x + sun.z * sun.z);
            if (horiz > 1.0e-4f && sun.y > 0.02f) // only while the sun is meaningfully above the horizon
            {
                // Shadows fall AWAY from the sun. Low elevation (small sun.y)
                // -> longer offset, clamped to kMaxSunOffsetM so dawn/dusk
                // never flings the blob off its owner.
                float elevW = sun.y;
                if (elevW > 1.0f)
                    elevW = 1.0f;
                const float mag = kMaxSunOffsetM * (1.0f - elevW);
                sunOffX = -(sun.x / horiz) * mag;
                sunOffZ = -(sun.z / horiz) * mag;
            }
        }

        if (ctx.players)
        {
            ctx.players->ForEachAlivePawn(
                [&](const PawnInfo& p) { Consider(ctx, p.pos, kPawnBlobRadiusM, camPos, sunOffX, sunOffZ); });
        }

        if (ctx.vehicles)
        {
            ctx.vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& v)
                { Consider(ctx, v.pos, VehicleBlobRadiusM(v.vehId), camPos, sunOffX, sunOffZ); });
        }

        if (ctx.deployables)
        {
            ctx.deployables->ForEachDeployable(
                [&](const TFDeployableView& d)
                {
                    const TFDeployableSpec* spec = TFDeploySpecOf(d.kind);
                    const float radius = spec ? spec->overlapRadiusM : kDeployableFallbackRadiusM;
                    Consider(ctx, d.pos, radius, camPos, sunOffX, sunOffZ);
                });
        }

        if (m_scratch.empty())
            return;

        if (!m_quad)
        {
            m_quad = std::make_unique<Mesh>();
            m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal -- already ground-flat, no billboard math needed
        }
        if (!m_quad || m_quad->GetIndexCount() == 0)
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(gfx->GetOrCreateSoftCircleShadowSRV());
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Alpha);
        // Depth READ-ONLY: blobs test against the scene (so they don't draw
        // through geometry behind them) but never write depth, so overlapping
        // blobs and anything drawn after this pass are unaffected.
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        for (const BlobDraw& b : m_scratch)
        {
            const XMMATRIX world = XMMatrixScaling(b.scale, 1.0f, b.scale) * XMMatrixTranslation(b.x, b.y, b.z);
            // ObjectColor.rgb == 0: the basic PS multiplies texColor.rgb by
            // this, so lighting/specular/emissive all zero out regardless of
            // sun angle -- the disc is pure unlit black; only its alpha (the
            // soft-circle texture's alpha times MaterialProperties.w below)
            // shapes it. No roughness map is bound, so the default fully-rough
            // t2 binding keeps specular exactly zero too (see the engine-side
            // comment in GetOrCreateSoftCircleShadowSRV).
            gfx->UpdateBasicConstants(world, view, proj, XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f), {1.0f, 1.0f},
                                      /*emissive*/ 0.0f, /*alpha*/ b.alpha, XMFLOAT2(0.0f, 0.0f));
            m_quad->Render(dc);
        }

        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
