/**
 * @file TFGroundFx.cpp
 * @brief Hover-dust flipbook emitter (see TFGroundFx.h for the wiring
 *        contract). Emission is data-derived from vehicle position deltas;
 *        rendering reuses the basic-shader flipbook path (UVTiling.xy frame
 *        window + UVTiling.zw offset) with additive blending.
 */
#include "Game/TFGroundFx.h"

#include "Game/TFVehicleSystem.h"
#include "World/TFWorldSetup.h"

#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Terrafront
{

    namespace
    {

        // ---------------------------------------------------------------- tuning
        constexpr float kMaxFrameDtSec = 0.05f; ///< clamp render dt (alt-tab / hitches)

        // Emission.
        constexpr float kMinDustSpeedMps = 3.0f;    ///< below this a vehicle kicks no dust
        constexpr float kSpawnPerSecPerMps = 0.55f; ///< rate proportional to speed (~11/s at 20 m/s)
        constexpr size_t kMaxPuffs = 64;            ///< hard process-wide cap
        constexpr float kMaxEmitDistM = 200.0f;     ///< skip emitters far from the camera
        constexpr float kSpeedSmoothPerSec = 8.0f;  ///< speed low-pass (mirror poses step at 20 Hz)

        // Puff shape / motion.
        constexpr float kPuffLifeBaseSec = 0.45f;
        constexpr float kPuffLifeJitterSec = 0.30f;
        constexpr float kPuffLiftM = 0.15f;    ///< spawn height above the terrain sample
        constexpr float kPuffSize0M = 0.55f;   ///< billboard edge at birth
        constexpr float kPuffSize1M = 1.70f;   ///< ... and at death (grows as it fades)
        constexpr float kPuffUpMps = 0.9f;     ///< base upward drift
        constexpr float kPuffOutMps = 0.7f;    ///< radial drift away from the hull
        constexpr float kPuffBackFrac = 0.12f; ///< of vehicle speed, trailed behind
        constexpr float kPuffBackCapMps = 2.5f;
        constexpr float kPuffAlpha = 0.5f;                    ///< peak additive weight
        constexpr float kDustColor[3] = {1.0f, 0.90f, 0.72f}; ///< sandy, matches Cindral terrain

        // Flipbook (4-frame horizontal strip, same trick as the muzzle flash).
        constexpr int kDustFrames = 4;
        constexpr char kDustSheet[] = "Assets/Textures/MMOFPS/fx/hover_dust.png";

        /// Approximate hull footprint radius for spawn scatter (visual only —
        /// authoritative radii live server-side in TFVehicleSystem).
        float HullRadiusM(VehicleId id)
        {
            switch (id)
            {
            case VehicleId::Drifter:
                return 1.2f;
            case VehicleId::Aegis:
                return 2.4f;
            case VehicleId::Ravager:
                return 2.2f;
            case VehicleId::Vulture:
                return 2.0f;
            default:
                return 1.6f;
            }
        }

        double NowRealSec()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

    } // namespace

    // ---------------------------------------------------------------------------

    TFGroundFx& TFGroundFx::Get()
    {
        static TFGroundFx s_instance;
        return s_instance;
    }

    float TFGroundFx::Rand01()
    {
        return static_cast<float>(m_rng() & 0xFFFFu) / 65535.0f;
    }

    void TFGroundFx::UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                     const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!gfx || !gfx->GetDevice() || !gfx->GetContext() || !ctx.world || !ctx.vehicles)
            return;

        // ------------------------------------------------------------ time step
        const double nowReal = NowRealSec();
        float dt = (m_lastRealTime < 0.0) ? 0.0f : static_cast<float>(nowReal - m_lastRealTime);
        m_lastRealTime = nowReal;
        dt = std::clamp(dt, 0.0f, kMaxFrameDtSec);
        ++m_frame;

        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR camPos = invView.r[3];

        // ------------------------------------------------------------ emission
        // Speed comes from per-entity position deltas so the same code covers
        // authoritative records (server roles) and interpolated mirrors (pure
        // clients). The low-pass keeps 20 Hz mirror steps from strobing the rate.
        ctx.vehicles->ForEachVehicle(
            [&](const TFVehicleInfo& v)
            {
                VehicleTrack& tr = m_tracks[v.entity];
                tr.lastSeenFrame = m_frame;

                if (tr.hasPrev && dt > 1.0e-4f)
                {
                    const float dx = v.pos[0] - tr.prevPos[0];
                    const float dz = v.pos[2] - tr.prevPos[2];
                    const float dist = std::sqrt(dx * dx + dz * dz);
                    const float blend = std::min(kSpeedSmoothPerSec * dt, 1.0f);
                    tr.speedMps += (dist / dt - tr.speedMps) * blend;
                    if (dist > 1.0e-3f)
                    {
                        tr.moveDir[0] = dx / dist;
                        tr.moveDir[1] = dz / dist;
                    }
                }
                tr.prevPos[0] = v.pos[0];
                tr.prevPos[1] = v.pos[1];
                tr.prevPos[2] = v.pos[2];
                tr.hasPrev = true;

                if (tr.speedMps < kMinDustSpeedMps)
                {
                    tr.spawnAccum = 0.0f; // no banked puffs while parked
                    return;
                }

                const XMVECTOR toCam = XMVectorSubtract(XMVectorSet(v.pos[0], v.pos[1], v.pos[2], 0.0f), camPos);
                if (XMVectorGetX(XMVector3LengthSq(toCam)) > kMaxEmitDistM * kMaxEmitDistM)
                    return;

                tr.spawnAccum += tr.speedMps * kSpawnPerSecPerMps * dt;
                const float hullR = HullRadiusM(v.vehId);
                while (tr.spawnAccum >= 1.0f && m_puffs.size() < kMaxPuffs)
                {
                    tr.spawnAccum -= 1.0f;
                    DustPuff p;
                    const float ang = Rand01() * 6.2831853f;
                    const float rad = (0.35f + 0.65f * Rand01()) * hullR;
                    p.pos[0] = v.pos[0] + std::cos(ang) * rad;
                    p.pos[2] = v.pos[2] + std::sin(ang) * rad;
                    p.pos[1] = ctx.world->TerrainHeightAt(p.pos[0], p.pos[2]) + kPuffLiftM;
                    const float back = std::min(tr.speedMps * kPuffBackFrac, kPuffBackCapMps);
                    p.vel[0] = std::cos(ang) * kPuffOutMps - tr.moveDir[0] * back;
                    p.vel[2] = std::sin(ang) * kPuffOutMps - tr.moveDir[1] * back;
                    p.vel[1] = kPuffUpMps * (0.6f + 0.8f * Rand01());
                    p.life = kPuffLifeBaseSec + kPuffLifeJitterSec * Rand01();
                    m_puffs.push_back(p);
                }
                if (tr.spawnAccum >= 1.0f)
                    tr.spawnAccum = 0.0f; // cap hit — drop the backlog, don't burst later
            });

        // Prune tracks for vehicles that stopped being enumerated (destroyed /
        // dropped from the mirror). Every 64 frames is plenty.
        if ((m_frame & 63u) == 0u)
        {
            std::erase_if(m_tracks, [&](const auto& kv) { return kv.second.lastSeenFrame != m_frame; });
        }

        // ------------------------------------------------------------ advance
        for (DustPuff& p : m_puffs)
        {
            p.age += dt;
            p.pos[0] += p.vel[0] * dt;
            p.pos[1] += p.vel[1] * dt;
            p.pos[2] += p.vel[2] * dt;
        }
        std::erase_if(m_puffs, [](const DustPuff& p) { return p.age >= p.life; });
        if (m_puffs.empty())
            return;

        // ------------------------------------------------------------ render
        if (!m_quad)
        {
            m_quad = std::make_unique<Mesh>();
            m_quad->Initialize(gfx->GetDevice(), gfx->GetContext());
            m_quad->CreatePlane(1.0f, 1.0f); // XZ plane, +Y normal, 0..1 UVs
        }
        if (!m_quad || m_quad->GetIndexCount() == 0)
            return;

        ID3D11DeviceContext* dc = gfx->GetContext();
        gfx->SetBasicShaders();
        gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(kDustSheet));
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Additive);
        // Depth READ-ONLY (W9): puffs test against the scene but never write,
        // so overlapping additive billboards can't occlude each other or
        // anything drawn after this pass.
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        // Billboard basis from inv(view) rows (camera axes in world space): the
        // plane's local X -> camera right, local Z -> camera up (v=0 edge is +Z,
        // keeping the sheet's top screen-up), local Y (front face) -> toward the
        // camera. Determinant stays +1, so CreatePlane's winding remains
        // front-facing under back-face cull.
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

        for (const DustPuff& p : m_puffs)
        {
            const float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
            const float size = kPuffSize0M + (kPuffSize1M - kPuffSize0M) * t;
            const int frame = std::min(kDustFrames - 1, static_cast<int>(t * kDustFrames));
            const float fade = kPuffAlpha * (1.0f - t) * (1.0f - t); // ease-out

            XMMATRIX bb = XMMatrixIdentity();
            bb.r[0] = XMVectorScale(right, size);
            bb.r[1] = toward;
            bb.r[2] = XMVectorScale(up, size);
            bb.r[3] = XMVectorSet(p.pos[0], p.pos[1], p.pos[2], 1.0f);

            // Fully emissive (unlit dust reads at any sun angle); fade rides the
            // alpha term, which the additive blend (SRC_ALPHA + ONE) weights by.
            gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(kDustColor[0], kDustColor[1], kDustColor[2], 1.0f),
                                      {1.0f / kDustFrames, 1.0f}, /*emissive*/ 1.0f, /*alpha*/ fade,
                                      XMFLOAT2(static_cast<float>(frame) / kDustFrames, 0.0f));
            m_quad->Render(dc);
        }

        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
