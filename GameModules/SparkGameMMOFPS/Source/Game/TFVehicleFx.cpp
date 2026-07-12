/**
 * @file TFVehicleFx.cpp
 * @brief Vehicle damage-state flipbook emitter + critical-tier engine audio
 *        cue (see TFVehicleFx.h for the wiring/tier contract). Emission is
 *        data-derived from TFVehicleInfo.hp/maxHp -- works identically on
 *        authoritative records (server roles) and interpolated replication
 *        mirrors (pure clients), same as TFGroundFx's speed-from-position-
 *        deltas trick.
 */
#include "Game/TFVehicleFx.h"

#include "Data/TFDataTables.h"
#include "Game/TFVehicleSystem.h"

#include "Audio/AudioEngine.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
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
        constexpr size_t kMaxQuads = 160;       ///< hard process-wide cap across all kinds

        // Damage tiers -- INTENTIONALLY the same numeric literals as
        // TFVehicleSystem.cpp's kTFVehDamagedHpFrac/kTFVehCriticalHpFrac so the
        // visuals and the server-authoritative speed/turn penalty change state
        // at the same hp fraction (see TFVehicleFx.h header comment).
        constexpr float kDamagedHpFrac = 0.66f;
        constexpr float kCriticalHpFrac = 0.33f;

        // Smoke (alpha-blended; tint carries the damaged/critical distinction --
        // the sheet itself is white + alpha-only, same trick as hover_dust).
        constexpr float kSmokeRateDamaged = 2.0f;  // puffs/sec
        constexpr float kSmokeRateCritical = 5.0f; // puffs/sec
        constexpr float kSmokeLiftM = 1.1f;        // spawn height above the hull base
        constexpr float kSmokeScatterM = 0.6f;     // horizontal jitter around the lift point
        constexpr float kSmokeUpMps = 1.3f;
        constexpr float kSmokeDamagedColor[3] = {0.62f, 0.62f, 0.62f};  // light gray trail
        constexpr float kSmokeCriticalColor[3] = {0.05f, 0.05f, 0.05f}; // heavy black smoke

        // Fire (critical only; additive engine-bay flicker).
        constexpr float kFireRate = 7.0f; // licks/sec
        constexpr float kFireLiftM = 0.8f;
        constexpr float kFireColor[3] = {1.0f, 0.42f, 0.10f};

        // Spark (damaged tier; occasional, additive).
        constexpr float kSparkCooldownMinSec = 2.0f;
        constexpr float kSparkCooldownMaxSec = 4.0f;
        constexpr float kSparkColor[3] = {1.0f, 0.92f, 0.62f};

        // Critical-tier engine-audio pitch-down: a periodic one-shot of the
        // vehicle's own vehicles.json `audioEngine` clip (loaded but never
        // played before this lane) at a lowered pitch -- reads as a labored
        // engine without needing to manage a persistent looping voice handle.
        constexpr float kEngineCuePeriodSec = 1.1f;
        constexpr float kEngineCuePitch = 0.62f;
        constexpr float kEngineCueVolume = 0.5f;

        // Flipbook sheets (4-frame horizontal strips, same trick as TFGroundFx /
        // TFImpactFx / the TFViewModel muzzle flash).
        constexpr int kFxFrames = 4;
        constexpr char kSmokeSheet[] = "Assets/Textures/MMOFPS/fx/veh_smoke.png";
        constexpr char kFireSparkSheet[] = "Assets/Textures/MMOFPS/fx/muzzle_flash_common.png"; // reused (fire+spark)

        double NowRealSec()
        {
            using clock = std::chrono::steady_clock;
            return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
        }

    } // namespace

    // ---------------------------------------------------------------------------

    TFVehicleFx& TFVehicleFx::Get()
    {
        static TFVehicleFx s_instance;
        return s_instance;
    }

    float TFVehicleFx::Rand01()
    {
        return static_cast<float>(m_rng() & 0xFFFFu) / 65535.0f;
    }

    void TFVehicleFx::Spawn(Kind kind, const float pos[3], const float vel[3], float life, float size0, float size1,
                            float alpha, const float color[3])
    {
        if (m_quads.size() >= kMaxQuads)
            return; // hard cap -- drop rather than grow unbounded
        FxQuad q;
        q.pos[0] = pos[0];
        q.pos[1] = pos[1];
        q.pos[2] = pos[2];
        q.vel[0] = vel[0];
        q.vel[1] = vel[1];
        q.vel[2] = vel[2];
        q.age = 0.0f;
        q.life = life;
        q.size0 = size0;
        q.size1 = size1;
        q.alpha = alpha;
        q.color[0] = color[0];
        q.color[1] = color[1];
        q.color[2] = color[2];
        q.kind = kind;
        m_quads.push_back(q);
    }

    void TFVehicleFx::PlayEngineCue(TFGameContext& ctx, EntityId vehicle, VehicleId vehId, const float pos[3])
    {
        (void)vehicle;
        if (!ctx.engine || !ctx.data || !ctx.data->IsLoaded())
            return;
        const VehicleDef* def = ctx.data->GetVehicle(vehId);
        if (!def || def->audioEngine.empty())
            return;
        ::AudioEngine* audio = ctx.engine->GetAudio();
        if (!audio)
            return;
        if (m_loadedEngineSounds.insert(def->audioEngine).second)
        {
            const std::string full = "Assets/" + def->audioEngine; // vehicles.json paths are Assets-relative
            audio->LoadSound(def->audioEngine, std::wstring(full.begin(), full.end()));
        }
        audio->PlaySound3D(def->audioEngine, DirectX::XMFLOAT3(pos[0], pos[1], pos[2]), kEngineCueVolume,
                           kEngineCuePitch, /*loop*/ false);
    }

    void TFVehicleFx::UpdateAndRender(TFGameContext& ctx, GraphicsEngine* gfx, const DirectX::XMMATRIX& view,
                                      const DirectX::XMMATRIX& proj)
    {
        using namespace DirectX;

        if (!gfx || !gfx->GetDevice() || !gfx->GetContext() || !ctx.vehicles)
            return;

        // ------------------------------------------------------------ time step
        const double nowReal = NowRealSec();
        float dt = (m_lastRealTime < 0.0) ? 0.0f : static_cast<float>(nowReal - m_lastRealTime);
        m_lastRealTime = nowReal;
        dt = std::clamp(dt, 0.0f, kMaxFrameDtSec);
        ++m_frame;

        // ------------------------------------------------------------ emission
        // hp/maxHp come straight off TFVehicleInfo -- identical on authoritative
        // records and interpolated mirrors, no TFVehicleSystem API changes.
        ctx.vehicles->ForEachVehicle(
            [&](const TFVehicleInfo& v)
            {
                if (v.hp <= 0.0f || v.maxHp <= 0.0f)
                    return; // dead hulls are wrecks now (TFVehicleSystem), not this system's concern
                const float frac = v.hp / v.maxHp;
                if (frac > kDamagedHpFrac)
                    return; // healthy -- no smoke/fire/spark

                VehicleTrack& tr = m_tracks[v.entity];
                tr.lastSeenFrame = m_frame;
                if (tr.sparkCooldown < 0.0f)
                    tr.sparkCooldown = kSparkCooldownMinSec + Rand01() * (kSparkCooldownMaxSec - kSparkCooldownMinSec);

                const bool critical = frac <= kCriticalHpFrac;
                const float smokeLift[3] = {v.pos[0], v.pos[1] + kSmokeLiftM, v.pos[2]};

                // -- smoke (both damaged and critical, rate/size/tint scale up) --
                tr.smokeAccum += (critical ? kSmokeRateCritical : kSmokeRateDamaged) * dt;
                while (tr.smokeAccum >= 1.0f)
                {
                    tr.smokeAccum -= 1.0f;
                    const float jx = (Rand01() - 0.5f) * 2.0f * kSmokeScatterM;
                    const float jz = (Rand01() - 0.5f) * 2.0f * kSmokeScatterM;
                    const float p[3] = {smokeLift[0] + jx, smokeLift[1], smokeLift[2] + jz};
                    const float vel[3] = {jx * 0.15f, kSmokeUpMps * (0.7f + 0.6f * Rand01()), jz * 0.15f};
                    if (critical)
                        Spawn(Kind::Smoke, p, vel, 1.3f + 0.3f * Rand01(), 0.9f, 2.6f, 0.55f, kSmokeCriticalColor);
                    else
                        Spawn(Kind::Smoke, p, vel, 1.0f + 0.3f * Rand01(), 0.6f, 1.8f, 0.32f, kSmokeDamagedColor);
                }

                if (critical)
                {
                    // -- fire flicker: frequent short-lived additive licks read as
                    //    continuous flame without a dedicated "is burning" state.
                    tr.fireAccum += kFireRate * dt;
                    const float fireAt[3] = {v.pos[0], v.pos[1] + kFireLiftM, v.pos[2]};
                    while (tr.fireAccum >= 1.0f)
                    {
                        tr.fireAccum -= 1.0f;
                        const float jx = (Rand01() - 0.5f) * 0.5f;
                        const float jz = (Rand01() - 0.5f) * 0.5f;
                        const float p[3] = {fireAt[0] + jx, fireAt[1], fireAt[2] + jz};
                        const float vel[3] = {0.0f, 0.6f, 0.0f};
                        Spawn(Kind::Fire, p, vel, 0.16f + 0.10f * Rand01(), 0.30f + 0.15f * Rand01(), 0.7f, 0.85f,
                             kFireColor);
                    }

                    // -- engine-audio pitch-down: periodic one-shot, no persistent
                    //    voice to manage/leak. Server-authoritative degradation
                    //    (TFVehicleSystem::DamageMovementMults) already applies
                    //    the actual speed/turn penalty; this is presentation only.
                    tr.engineCueCooldown -= dt;
                    if (tr.engineCueCooldown <= 0.0f && ctx.HasLocalPlayer())
                    {
                        tr.engineCueCooldown = kEngineCuePeriodSec;
                        PlayEngineCue(ctx, v.entity, v.vehId, v.pos);
                    }
                }
                else
                {
                    // -- occasional spark (damaged tier only) --
                    tr.sparkCooldown -= dt;
                    if (tr.sparkCooldown <= 0.0f)
                    {
                        tr.sparkCooldown =
                            kSparkCooldownMinSec + Rand01() * (kSparkCooldownMaxSec - kSparkCooldownMinSec);
                        const float p[3] = {smokeLift[0], v.pos[1] + 0.9f, smokeLift[2]};
                        const float vel[3] = {0.0f, 0.4f, 0.0f};
                        Spawn(Kind::Spark, p, vel, 0.14f, 0.24f, 0.4f, 0.9f, kSparkColor);
                    }
                }
            });

        // Prune tracks for vehicles that stopped being enumerated (healed back
        // to full, destroyed, or dropped from the mirror). Every 64 frames is
        // plenty, matches TFGroundFx.
        if ((m_frame & 63u) == 0u)
        {
            std::erase_if(m_tracks, [&](const auto& kv) { return kv.second.lastSeenFrame != m_frame; });
        }

        // ------------------------------------------------------------ advance
        for (FxQuad& q : m_quads)
        {
            q.age += dt;
            q.pos[0] += q.vel[0] * dt;
            q.pos[1] += q.vel[1] * dt;
            q.pos[2] += q.vel[2] * dt;
        }
        std::erase_if(m_quads, [](const FxQuad& q) { return q.age >= q.life; });
        if (m_quads.empty())
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
        // Depth READ-ONLY (TFGroundFx/TFImpactFx convention): billboards test
        // against the scene but never write, so overlapping FX can't occlude
        // each other or anything drawn after this pass.
        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);

        // Billboard basis from inv(view) rows -- identical construction to
        // TFGroundFx/TFImpactFx.
        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        const XMVECTOR right = XMVector3Normalize(invView.r[0]);
        const XMVECTOR up = XMVector3Normalize(invView.r[1]);
        const XMVECTOR toward = XMVectorNegate(XMVector3Normalize(invView.r[2]));

        const auto drawBatch = [&](Kind kind, const char* sheet, GraphicsEngine::BasicBlendMode blend)
        {
            bool bound = false;
            for (const FxQuad& q : m_quads)
            {
                if (q.kind != kind)
                    continue;
                if (!bound)
                {
                    gfx->SetBasicTexture(gfx->GetOrLoadTextureSRV(sheet));
                    gfx->SetBasicBlendMode(blend);
                    bound = true;
                }

                const float t = std::clamp(q.age / q.life, 0.0f, 1.0f);
                const float size = q.size0 + (q.size1 - q.size0) * t;
                const int frame = std::min(kFxFrames - 1, static_cast<int>(t * kFxFrames));
                const float fade = q.alpha * (1.0f - t) * (1.0f - t); // ease-out, matches TFGroundFx

                XMMATRIX bb = XMMatrixIdentity();
                bb.r[0] = XMVectorScale(right, size);
                bb.r[1] = toward;
                bb.r[2] = XMVectorScale(up, size);
                bb.r[3] = XMVectorSet(q.pos[0], q.pos[1], q.pos[2], 1.0f);

                gfx->UpdateBasicConstants(bb, view, proj, XMFLOAT4(q.color[0], q.color[1], q.color[2], 1.0f),
                                          {1.0f / kFxFrames, 1.0f}, /*emissive*/ 1.0f, /*alpha*/ fade,
                                          XMFLOAT2(static_cast<float>(frame) / kFxFrames, 0.0f));
                m_quad->Render(dc);
            }
        };

        // Smoke first (alpha), fire + spark after (additive, shared reused
        // sheet -- drawBatch rebinds the same SRV/blend for the second call,
        // which is a no-op state change but keeps the batching logic in one
        // place rather than duplicating the per-quad draw loop).
        drawBatch(Kind::Smoke, kSmokeSheet, GraphicsEngine::BasicBlendMode::Alpha);
        drawBatch(Kind::Fire, kFireSparkSheet, GraphicsEngine::BasicBlendMode::Additive);
        drawBatch(Kind::Spark, kFireSparkSheet, GraphicsEngine::BasicBlendMode::Additive);

        gfx->SetBasicDepthMode(GraphicsEngine::BasicDepthMode::Default);
        gfx->SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Opaque);
        gfx->SetBasicTexture(nullptr);
    }

} // namespace Terrafront
