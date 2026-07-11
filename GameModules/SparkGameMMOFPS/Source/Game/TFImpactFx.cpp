/**
 * @file TFImpactFx.cpp
 * @brief Bullet-impact flipbook emitter (see TFImpactFx.h for the wiring and
 *        budget contract). Rendering reuses the basic-shader flipbook path
 *        (UVTiling.xy frame window + UVTiling.zw offset), one blend batch per
 *        surface flavor: dust = alpha, spark = additive, both depth read-only.
 */
#include "Game/TFImpactFx.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFFireFxProtocol.h" // W11 impact-broadcast: TFImpactSurface + kTFImpactFxRangeM
#include "World/TFWorldSetup.h"

#include "Camera/SparkEngineCamera.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Physics/PhysicsSystem.h" // engine umbrella header; stub-safe when Jolt is absent
#include "Spark/IEngineContext.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Terrafront
{

    namespace
    {

        // ---------------------------------------------------------------- tuning
        constexpr float kMaxFrameDtSec = 0.05f; ///< clamp render dt (alt-tab / hitches)

        // Shared burst timing (task spec: brief, ~0.15 s).
        constexpr float kImpactLifeBaseSec = 0.15f;
        constexpr float kImpactLifeJitterSec = 0.04f;

        // Dust (terrain / static world): dirt puff, alpha-blended — additive
        // washes out against the bright sand; straight alpha reads as a kicked-up
        // plume at any sun angle.
        constexpr float kDustSize0M = 0.45f;                  ///< billboard edge at birth
        constexpr float kDustGrow = 1.9f;                     ///< edge multiplier at death
        constexpr float kDustAlpha = 0.62f;                   ///< peak blend weight
        constexpr float kDustColor[3] = {1.0f, 0.90f, 0.72f}; ///< sandy, matches TFGroundFx / Cindral terrain

        // Spark (pawn / vehicle): smaller, hotter, additive.
        constexpr float kSparkSize0M = 0.30f;
        constexpr float kSparkGrow = 0.55f; ///< sparks collapse rather than billow
        constexpr float kSparkAlpha = 0.9f;
        constexpr float kSparkColor[3] = {1.0f, 0.93f, 0.70f};

        // Flipbook (both sheets are 4-frame horizontal strips, same trick as the
        // TFViewModel muzzle flash and TFGroundFx).
        constexpr int kFxFrames = 4;
        constexpr char kDustSheet[] = "Assets/Textures/MMOFPS/fx/hover_dust.png";
        constexpr char kSparkSheet[] = "Assets/Textures/MMOFPS/fx/muzzle_flash_common.png";

        // Trace shape.
        constexpr float kMuzzleClearM =
            0.5f; ///< skip near-origin blocks (own capsule/hull), = Ballistics::kMuzzleClearanceM
        constexpr float kTerrainStepM = 1.0f;  ///< analytic march step (matches TFWeaponSystem::TerrainBlocked)
        constexpr float kSurfacePullM = 0.06f; ///< pull the quad off the surface toward the shooter (no z-fight)
        constexpr float kVehiclePadM = 0.5f;   ///< sphere fudge over the visual hull radius
        constexpr float kVehicleLiftM = 0.9f;  ///< sphere center height over the (ground-level) vehicle pos

        /// Approximate hull radius for the vehicle-sphere flavor test (visual
        /// only — authoritative shapes live server-side; mirrors TFGroundFx).
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

        /// Smallest positive ray-sphere intersection distance, or a negative
        /// value when the ray misses (ray dir must be unit length).
        float RaySphere(const float o[3], const float d[3], const float c[3], float r)
        {
            const float oc[3] = {o[0] - c[0], o[1] - c[1], o[2] - c[2]};
            const float b = WeaponMath::Dot3(oc, d);
            const float q = WeaponMath::Dot3(oc, oc) - r * r;
            const float disc = b * b - q;
            if (disc < 0.0f)
                return -1.0f;
            const float s = std::sqrt(disc);
            const float t0 = -b - s;
            if (t0 > 0.0f)
                return t0;
            return -b + s; // origin inside the sphere: exit point (still a hit)
        }

        double NowRealSec()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

    } // namespace

    // ---------------------------------------------------------------------------

    TFImpactFx& TFImpactFx::Get()
    {
        static TFImpactFx s_instance;
        return s_instance;
    }

    float TFImpactFx::Rand01()
    {
        return static_cast<float>(m_rng() & 0xFFFFu) / 65535.0f;
    }

    bool TFImpactFx::CameraPos(TFGameContext& ctx, float out[3])
    {
        // Module-owned camera (the engine-context camera slot is empty in module
        // mode — see TFWorldSetup::GetCamera), pawn-eye fallback like
        // TFWeaponSystem::LocalListenerPos.
        if (ctx.world)
        {
            if (const SparkEngineCamera* cam = ctx.world->GetCamera())
            {
                const auto& p = cam->GetPosition();
                out[0] = p.x;
                out[1] = p.y;
                out[2] = p.z;
                return true;
            }
        }
        PawnInfo pawn;
        if (ctx.players && ctx.players->GetPawnByPlayer(ctx.localPlayer, pawn))
        {
            out[0] = pawn.pos[0];
            out[1] = pawn.pos[1] + WeaponMath::kEyeHeightM;
            out[2] = pawn.pos[2];
            return true;
        }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Impact point resolution
    // ---------------------------------------------------------------------------

    bool TFImpactFx::TraceImpact(TFGameContext& ctx, const float origin[3], const float dir[3], float maxDist,
                                 bool includePawns, float outPoint[3], Kind& outKind) const
    {
        if (!ctx.world || maxDist <= kMuzzleClearM)
            return false;

        float d[3] = {dir[0], dir[1], dir[2]};
        if (!WeaponMath::Normalize3(d))
            return false;

        float bestT = maxDist;
        bool hit = false;
        Kind kind = Kind::Dust;

        // 1) Physics ray against live Jolt bodies. GetJoltSystem() probe: the
        //    no-Jolt stub hands out valid dummy bodies, so the Jolt world itself
        //    is the only trustworthy signal (TFWorldCollision precedent).
        ::PhysicsSystem* physics = ctx.engine ? ctx.engine->GetPhysics() : nullptr;
        if (physics && physics->GetJoltSystem())
        {
            const uint16_t mask =
                static_cast<uint16_t>(includePawns ? CollisionLayers::HitscanMask
                                                   : (CollisionLayers::WorldStatic | CollisionLayers::Vehicle));
            const DirectX::XMFLOAT3 o{origin[0] + d[0] * kMuzzleClearM, origin[1] + d[1] * kMuzzleClearM,
                                      origin[2] + d[2] * kMuzzleClearM};
            const RaycastHit rh = physics->RaycastFiltered(o, {d[0], d[1], d[2]}, maxDist - kMuzzleClearM, mask);
            if (rh.hasHit && rh.distance + kMuzzleClearM < bestT)
            {
                bestT = rh.distance + kMuzzleClearM;
                hit = true;
                // Flavor by what the entity id resolves to in the client mirrors
                // (pawn/vehicle body -> spark, anything else -> world dust).
                PawnInfo hp;
                TFVehicleInfo hv;
                const bool isPawn = rh.entityId != 0 && ctx.players && ctx.players->GetPawnByEntity(rh.entityId, hp);
                const bool isVehicle =
                    rh.entityId != 0 && !isPawn && ctx.vehicles && ctx.vehicles->GetVehicleInfo(rh.entityId, hv);
                kind = (isPawn || isVehicle) ? Kind::Spark : Kind::Dust;
            }
        }

        // 2) Analytic terrain march (identical fallback shape to
        //    TFWeaponSystem::TerrainBlocked), refined by bisection so the puff
        //    sits on the surface rather than up to 1 m past it.
        for (float t = kTerrainStepM; t < bestT; t += kTerrainStepM)
        {
            const float x = origin[0] + d[0] * t;
            const float y = origin[1] + d[1] * t;
            const float z = origin[2] + d[2] * t;
            if (y < ctx.world->TerrainHeightAt(x, z))
            {
                float lo = t - kTerrainStepM;
                float hi = t;
                for (int i = 0; i < 5; ++i)
                {
                    const float mid = 0.5f * (lo + hi);
                    const float my = origin[1] + d[1] * mid;
                    if (my < ctx.world->TerrainHeightAt(origin[0] + d[0] * mid, origin[2] + d[2] * mid))
                        hi = mid;
                    else
                        lo = mid;
                }
                bestT = 0.5f * (lo + hi);
                hit = true;
                kind = Kind::Dust;
                break;
            }
        }

        // 3) Client-mirror pawn capsules (local trace only — this is the spark
        //    flavor for the shooter's own crosshair, not hit registration).
        //    Clipped to the world hit found above so pawns behind cover are
        //    excluded; the shooter's own capsule is skipped.
        if (includePawns && ctx.players)
        {
            EntityId selfEntity = 0;
            PawnInfo self;
            if (ctx.HasLocalPlayer() && ctx.players->GetPawnByPlayer(ctx.localPlayer, self))
                selfEntity = self.entity;

            const float segEnd[3] = {origin[0] + d[0] * bestT, origin[1] + d[1] * bestT, origin[2] + d[2] * bestT};
            const float segLen = bestT;
            float bestPawnT = bestT;
            ctx.players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    if (p.entity == selfEntity)
                        return;
                    float t01 = 0.0f;
                    if (!WeaponMath::SegmentHitsCapsule(origin, segEnd, p.pos, WeaponMath::kPawnHeightM,
                                                        WeaponMath::kPawnRadiusM, t01))
                        return;
                    const float tM = t01 * segLen;
                    if (tM > kMuzzleClearM && tM < bestPawnT)
                        bestPawnT = tM;
                });
            if (bestPawnT < bestT)
            {
                bestT = bestPawnT;
                hit = true;
                kind = Kind::Spark;
            }
        }

        // 4) Vehicle mirror spheres: pure clients have no vehicle physics
        //    bodies, so the analytic test keeps local-prediction vehicle hits
        //    sparking. Clipped to the current bestT.
        if (ctx.vehicles)
        {
            float bestVehT = bestT;
            ctx.vehicles->ForEachVehicle(
                [&](const TFVehicleInfo& v)
                {
                    const float c[3] = {v.pos[0], v.pos[1] + kVehicleLiftM, v.pos[2]};
                    const float t = RaySphere(origin, d, c, HullRadiusM(v.vehId) + kVehiclePadM);
                    if (t > kMuzzleClearM && t < bestVehT)
                        bestVehT = t;
                });
            if (bestVehT < bestT)
            {
                bestT = bestVehT;
                hit = true;
                kind = Kind::Spark;
            }
        }

        if (!hit)
            return false;

        // Pull the quad slightly back toward the shooter so it never z-fights
        // the surface it decorates.
        const float tOut = std::max(0.0f, bestT - kSurfacePullM);
        outPoint[0] = origin[0] + d[0] * tOut;
        outPoint[1] = origin[1] + d[1] * tOut;
        outPoint[2] = origin[2] + d[2] * tOut;
        outKind = kind;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Producers
    // ---------------------------------------------------------------------------

    void TFImpactFx::OnLocalShot(TFGameContext& ctx, const float origin[3], const float dir[3])
    {
        if (!ctx.HasLocalPlayer() || !ctx.world)
            return;

        float point[3];
        Kind kind = Kind::Dust;
        if (TraceImpact(ctx, origin, dir, kTraceBudgetM, /*includePawns*/ true, point, kind))
            Spawn(point, kind);
    }

    bool TFImpactFx::OnServerImpact(TFGameContext& ctx, const float point[3], uint8_t surface)
    {
        // W11 impact-broadcast: the point is server truth (0x54F5 wire or the
        // in-process listen-host route) — no trace, just the flavored puff.
        if (!ctx.HasLocalPlayer())
            return false;

        // Camera gate: the server already range-gates recipients by PAWN pos;
        // this re-gate covers the in-process route and free-flying cameras.
        float cam[3];
        if (!CameraPos(ctx, cam))
            return false;
        if (WeaponMath::Dist2(point, cam) > kTFImpactFxRangeM * kTFImpactFxRangeM)
            return false;

        const auto s = static_cast<TFImpactSurface>(surface);
        const Kind kind = (s == TFImpactSurface::Terrain || s == TFImpactSurface::Static) ? Kind::Dust : Kind::Spark;
        Spawn(point, kind);
        return true;
    }

    void TFImpactFx::Spawn(const float point[3], Kind kind)
    {
        // Pooled: reuse a dead slot; when all 48 are live, steal the one closest
        // to death (never allocate, never exceed the cap).
        ImpactQuad* slot = nullptr;
        float bestFrac = -1.0f;
        for (ImpactQuad& q : m_pool)
        {
            if (!q.alive)
            {
                slot = &q;
                break;
            }
            const float frac = q.age / q.life;
            if (frac > bestFrac)
            {
                bestFrac = frac;
                slot = &q;
            }
        }

        ImpactQuad q;
        q.pos[0] = point[0];
        q.pos[1] = point[1];
        q.pos[2] = point[2];
        q.age = 0.0f;
        q.life = kImpactLifeBaseSec + kImpactLifeJitterSec * Rand01();
        q.kind = kind;
        q.size = (kind == Kind::Dust ? kDustSize0M : kSparkSize0M) * (0.85f + 0.3f * Rand01());
        q.alive = true;
        *slot = q;
    }

    // ---------------------------------------------------------------------------
    // Frame advance + render
    // ---------------------------------------------------------------------------

    void TFImpactFx::UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                     const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;
        (void)ctx; // reserved for future context-dependent culling; keeps the TFGroundFx-shaped signature

        if (!gfx || !gfx->GetDevice() || !gfx->GetContext())
            return;

        // ------------------------------------------------------------ time step
        const double nowReal = NowRealSec();
        float dt = (m_lastRealTime < 0.0) ? 0.0f : static_cast<float>(nowReal - m_lastRealTime);
        m_lastRealTime = nowReal;
        dt = std::clamp(dt, 0.0f, kMaxFrameDtSec);

        bool anyAlive = false;
        for (ImpactQuad& q : m_pool)
        {
            if (!q.alive)
                continue;
            q.age += dt;
            if (q.age >= q.life)
                q.alive = false;
            else
                anyAlive = true;
        }
        if (!anyAlive)
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
        // Depth READ-ONLY: impacts test against the scene but never write, so
        // overlapping FX billboards can't occlude each other or later passes.
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        // Billboard basis from inv(view) rows (camera axes in world space) —
        // identical construction to TFGroundFx: local X -> camera right, local
        // Z -> camera up (v=0 edge stays screen-up), local Y -> toward the
        // camera; determinant +1 keeps CreatePlane's winding front-facing.
        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

        const auto drawBatch = [&](Kind kind, const char* sheet, GraphicsEngine::BasicBlendMode blend,
                                   const float color[3], float peakAlpha, float grow)
        {
            bool bound = false;
            for (const ImpactQuad& q : m_pool)
            {
                if (!q.alive || q.kind != kind)
                    continue;
                if (!bound)
                {
                    gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(sheet));
                    gfx->SetBasicBlendMode(blend);
                    bound = true;
                }

                const float t = std::clamp(q.age / q.life, 0.0f, 1.0f);
                const float size = q.size * (1.0f + (grow - 1.0f) * t);
                const int frame = std::min(kFxFrames - 1, static_cast<int>(t * kFxFrames));
                const float fade = peakAlpha * (1.0f - t); // linear out over 0.15 s reads as a pop

                XMMATRIX bb = XMMatrixIdentity();
                bb.r[0] = XMVectorScale(right, size);
                bb.r[1] = toward;
                bb.r[2] = XMVectorScale(up, size);
                bb.r[3] = XMVectorSet(q.pos[0], q.pos[1], q.pos[2], 1.0f);

                // Fully emissive (unlit FX reads at any sun angle); fade rides
                // the alpha term for both blend modes.
                gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(color[0], color[1], color[2], 1.0f),
                                          {1.0f / kFxFrames, 1.0f}, /*emissive*/ 1.0f, /*alpha*/ fade,
                                          XMFLOAT2(static_cast<float>(frame) / kFxFrames, 0.0f));
                m_quad->Render(dc);
            }
        };

        // Dust first (alpha), sparks after (additive) — grouped to keep blend
        // state changes to at most two per frame.
        drawBatch(Kind::Dust, kDustSheet, GraphicsEngine::BasicBlendMode::Alpha, kDustColor, kDustAlpha, kDustGrow);
        drawBatch(Kind::Spark, kSparkSheet, GraphicsEngine::BasicBlendMode::Additive, kSparkColor, kSparkAlpha,
                  kSparkGrow);

        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
