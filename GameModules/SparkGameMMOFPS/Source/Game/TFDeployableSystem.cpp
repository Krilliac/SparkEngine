/**
 * @file TFDeployableSystem.cpp
 * @brief Fabricator turret / ammo pack + Medtech beacon (W3) + resupply
 *        station / AV turret / shield wall (W6): server mechanics,
 *        0x54FC-0x54FE replication, client mirror + prop visuals. Placement
 *        validation + the ShieldWall Jolt body live in TFDeployablePlacement.cpp.
 *
 * Tick driving: Main.cpp (frozen) routes only Update() to this system, so all
 * mechanics run on the render tick with dt accumulation — none of them need
 * fixed-step determinism (timers, radius checks, hitscan through TFDamageSystem).
 * FixedUpdate stays a contract no-op so a future Main wiring cannot double-tick.
 */
#include "Game/TFDeployableSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableTypes.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFTransparentPass.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFVisualUtils.h"
#include "Net/TFRepProtocol.h"
#include "World/TFWorldSetup.h"

#include "Engine/ECS/Components.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace Terrafront
{

    namespace
    {

        // W3 kStats moved to Game/TFDeployableTypes.h (kTFDeployableSpecs) — one spec
        // table now carries stats + placement validation inputs for all six kinds.

        constexpr float kPlaceAheadM = 2.0f; // drop point in front of the pawn
        constexpr float kPlaceLiftM = 0.05f; // epsilon above terrain
        constexpr float kTurretRangeM = 40.0f;
        constexpr float kTurretDamage = 95.0f;
        constexpr float kTurretRpm = 450.0f;
        constexpr float kTurretMuzzleY = 1.5f; // fire point above base
        constexpr float kTurretAimY = 1.2f;    // aim at pawn chest height
        constexpr float kAmmoPackRadiusM = 5.0f;
        constexpr float kAmmoPackHealHp = 20.0f; // TF-W4: real ammo refill instead
        constexpr float kAmmoPackPulseSec = 5.0f;
        constexpr float kBeaconRadiusM = 6.0f;
        constexpr float kBeaconHealPerSec = 30.0f;
        constexpr float kKeepaliveSec = 1.0f;    // health/life refresh cadence
        constexpr uint8_t kDamageKindBullet = 0; // TF_DamageEvent convention

        // --- W6 kinds (specs in TFDeployableTypes.h; these are behavior tunings) ----
        constexpr float kResupplyRadiusM = 8.0f; // heal + repair reach
        constexpr float kResupplyPulseSec = 4.0f;
        constexpr float kResupplyHealHp = 25.0f;   // TF-W4: real ammo refill instead
        constexpr float kResupplyRepairHp = 40.0f; // per pulse, friendly deployables
        constexpr float kAVTurretRangeM = 60.0f;
        constexpr float kAVTurretDamage = 140.0f; // per shot vs vehicle hp pool
        constexpr float kAVTurretShotSec = 2.5f;  // slow heavy cadence
        constexpr float kAVTurretMuzzleY = 2.2f;  // fire point above base
        constexpr float kAVTurretAimY = 1.0f;     // aim at hull center height

        // Synthetic network ids when no ECS world exists (headless unit tests);
        // distinct base from TFPlayerSystem's 1000000 pawn range.
        EntityId g_nextSyntheticEntity = 2000000;

        constexpr float kRadToDeg = 57.2957795f;

        float Dist2(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        // --- W8 render-transparency lane: shield-wall energy plane --------------
        // Frame-local rectangle of the energy plane inside dep_shieldwall.obj
        // (meters, unscaled): posts at x = +-1.45 (inner clearance ~1.32), emitter
        // bar top at y ~0.34, post caps at y ~2.28. Multiplied by the SAME visual
        // scale the frame renders with so the plane always fills the frame.
        constexpr float kShieldPlaneHalfW = 1.30f;
        constexpr float kShieldPlaneY0 = 0.36f;
        constexpr float kShieldPlaneY1 = 2.18f;
        constexpr float kShieldPlaneEmissive = 0.6f;    // unlit glow (MaterialProperties.z)
        constexpr float kShieldPlaneAlphaMin = 0.28f;   // near-destroyed wall
        constexpr float kShieldPlaneAlphaMax = 0.55f;   // full-health wall
        constexpr float kShieldPlaneFactionMix = 0.65f; // white -> faction color blend
        constexpr const char* kShieldPlaneTexture = "Assets/Textures/MMOFPS/fx/shield_plane.png";

        /// Queue one shield wall's energy plane into the transparent pass (pure
        /// presentation; drawn by TFTransparentPass::Flush after all opaque
        /// passes — flush call site lives in TFWorldSetup::RenderWorld).
        void SubmitShieldWallPlane(TFGameContext& ctx, const TFDeployableView& view, Mesh* plane,
                                   ID3D11ShaderResourceView* srv)
        {
            using namespace DirectX;

            // Match the rendered frame's scale exactly: bespoke data row first,
            // else the aliased row * visualScaleMult — the same resolve order as
            // AttachDeployableVisual (never stack both).
            const DeployableVisualDef* dv = nullptr;
            bool aliased = false;
            if (ctx.data && ctx.data->IsLoaded())
            {
                dv = ctx.data->GetDeployableVisual(view.kind);
                if (!dv)
                {
                    dv = ctx.data->GetDeployableVisual(TFDeployVisualAlias(view.kind));
                    aliased = true;
                }
            }
            if (!dv)
                return; // no data tables -> no frame rendered -> no plane either
            const TFDeployableSpec* spec = TFDeploySpecOf(view.kind);
            const float sx = dv->scale[0] * ((aliased && spec) ? spec->visualScaleMult[0] : 1.0f);
            const float sy = dv->scale[1] * ((aliased && spec) ? spec->visualScaleMult[1] : 1.0f);

            const float w = 2.0f * kShieldPlaneHalfW * sx;
            const float h = (kShieldPlaneY1 - kShieldPlaneY0) * sy;
            const float cy = 0.5f * (kShieldPlaneY0 + kShieldPlaneY1) * sy;
            // Same composition as Transform::GetLocalMatrix (S * R * T); yaw stays
            // in RADIANS here (the ECS/PhysicsBody degree conversions are their
            // consumers' concern, not this draw's).
            const XMMATRIX world = XMMatrixScaling(w, h, 1.0f) * XMMatrixRotationY(view.yaw) *
                                   XMMatrixTranslation(view.pos[0], view.pos[1] + cy, view.pos[2]);

            // Faction-colored energy; alpha tracks health so a battered wall
            // visibly thins out before it pops.
            float fcol[4];
            FactionColor(view.faction, fcol);
            const float healthFrac =
                (view.maxHealth > 0.0f) ? std::clamp(view.health / view.maxHealth, 0.0f, 1.0f) : 1.0f;
            const float alpha = kShieldPlaneAlphaMin + (kShieldPlaneAlphaMax - kShieldPlaneAlphaMin) * healthFrac;
            const XMFLOAT4 tint{1.0f + kShieldPlaneFactionMix * (fcol[0] - 1.0f),
                                1.0f + kShieldPlaneFactionMix * (fcol[1] - 1.0f),
                                1.0f + kShieldPlaneFactionMix * (fcol[2] - 1.0f), alpha};

            TFTransparentPass::Get().Submit(plane, world, srv, tint, kShieldPlaneEmissive);
        }

    } // namespace

    TFDeployableSystem::TFDeployableSystem() = default;
    TFDeployableSystem::~TFDeployableSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDeployableSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFDeployableSystem initialized");
        return true;
    }

    void TFDeployableSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;

        if (m_ctx->IsAuthority())
        {
            ServerTick(deltaTime);
        }
        else
        {
            // client mirror: decay lifetimes locally between server keepalives
            for (auto& [entity, rec] : m_deployables)
                rec.view.life = std::max(0.0f, rec.view.life - deltaTime);
        }

        // --- W8 render-transparency: queue shield-wall energy planes ------------
        // Pure presentation, both roles (listen host draws its own authority set,
        // pure clients their replicated mirror); headless (no device) skips, so
        // nothing ever queues without a Flush. Lazy plane/SRV lookup only when a
        // wall actually exists this frame.
        if (GraphicsEngine* gfx = m_ctx->engine ? m_ctx->engine->GetGraphics() : nullptr;
            gfx && gfx->GetDevice() && gfx->GetContext())
        {
            Mesh* plane = nullptr;
            ID3D11ShaderResourceView* srv = nullptr;
            bool resolved = false;
            for (const auto& [entity, rec] : m_deployables)
            {
                if (rec.view.kind != kDeployShieldWall)
                    continue;
                if (!resolved)
                {
                    resolved = true;
                    plane = TFTransparentPass::Get().GetUnitPlane(*gfx);
                    srv = gfx->GetOrLoadTextureSRV(kShieldPlaneTexture);
                }
                if (!plane)
                    break; // quad creation failed — visual-only, skip quietly
                SubmitShieldWallPlane(*m_ctx, rec.view, plane, srv);
            }
        }

#ifdef ENABLE_NETWORKING
        if (ClientNetActive())
        {
            if (!m_clientHandlers)
                ClientEnsureHandlers();
        }
        else if (m_clientHandlers)
        {
            ClientReleaseHandlers();
            for (auto& [entity, rec] : m_deployables)
                DestroyLocalEntity(rec.local);
            m_deployables.clear();
        }
#endif
    }

    // Main.cpp does not drive FixedUpdate for this system; see file header.
    void TFDeployableSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFDeployableSystem::Shutdown()
    {
        if (!m_initialized)
            return;
#ifdef ENABLE_NETWORKING
        if (m_clientHandlers)
            ClientReleaseHandlers();
        m_knownClients.clear();
#endif
        for (auto& [entity, rec] : m_deployables)
        {
            ReleaseWallBody(rec);
            DestroyLocalEntity(rec.local);
        }
        m_deployables.clear();
        m_ownerIndex.clear();
        m_pendingAvShots.clear();
        m_initialized = false;
    }

    double TFDeployableSystem::NowSec() const
    {
        return m_clock;
    }

    // ---------------------------------------------------------------------------
    // Placement (server)
    // ---------------------------------------------------------------------------

    const char* TFDeployableSystem::ResultText(TFDeployResult r)
    {
        switch (r)
        {
        case TFDeployResult::Ok:
            return "deployed";
        case TFDeployResult::NotAuthority:
            return "server only";
        case TFDeployResult::DataMissing:
            return "data tables not loaded";
        case TFDeployResult::NoPawn:
            return "you must be alive";
        case TFDeployResult::WrongClass:
            return "your class cannot place that";
        case TFDeployResult::BadKind:
            return "unknown deployable kind";
        case TFDeployResult::SpawnFailed:
            return "placement failed";
        case TFDeployResult::TooSteep:
            return "ground too steep";
        case TFDeployResult::TooClose:
            return "too close to another deployable";
        case TFDeployResult::HostileRegion:
            return "cannot deploy in enemy territory";
        case TFDeployResult::Blocked:
            return "placement blocked by an obstacle";
        case TFDeployResult::LimitReached:
            return "deployable limit reached";
        }
        return "?";
    }

    TFDeployResult TFDeployableSystem::ServerTryPlaceDeployable(PlayerId player, DeployableKind kind)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return TFDeployResult::NotAuthority;
        const TFDeployableSpec* spec = TFDeploySpecOf(kind);
        if (!spec)
            return TFDeployResult::BadKind;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
            return TFDeployResult::NoPawn;

        // Class gate is spec-table-driven (W3's Fabricator/Medtech split, kept).
        if (static_cast<uint8_t>(pawn.cls) != spec->requiredClass)
            return TFDeployResult::WrongClass;

        // Per-player limits: at the PER-KIND cap the oldest of that kind is
        // replaced (the W3 one-per-kind rule, generalized); below it the TOTAL
        // active cap refuses outright.
        EntityId victim = 0;
        {
            auto& byKind = m_ownerIndex[player];
            const auto& sameKind = byKind[static_cast<uint8_t>(kind)];
            if (!sameKind.empty() && sameKind.size() >= static_cast<size_t>(spec->maxActive))
            {
                victim = sameKind.front(); // oldest first (placement order)
            }
            else
            {
                size_t total = 0;
                for (const auto& kv : byKind)
                    total += kv.second.size();
                if (total >= kTFDeployMaxActivePerPlayer)
                    return TFDeployResult::LimitReached;
            }
        }

        // Drop point: 2 m ahead on the pawn's facing (yaw convention: fwd = sin/cos,
        // see TFPlayerSystem::FindSkyanchorSpawn), clamped onto the terrain.
        float pos[3];
        pos[0] = pawn.pos[0] + std::sin(pawn.yaw) * kPlaceAheadM;
        pos[2] = pawn.pos[2] + std::cos(pawn.yaw) * kPlaceAheadM;
        pos[1] = (m_ctx->world ? m_ctx->world->TerrainHeightAt(pos[0], pos[2]) : pawn.pos[1]) + kPlaceLiftM;

        // W6: validate BEFORE any state changes (the to-be-replaced victim is
        // excluded from spacing — the new placement supersedes it).
        if (const TFDeployResult vr = ValidatePlacement(*spec, pawn.faction, pos, victim); vr != TFDeployResult::Ok)
        {
            ++m_refusedPlacements;
            return vr;
        }

        if (victim != 0)
            ServerDestroyDeployable(victim, "replaced");

        TFDeployableView view{};
        view.kind = kind;
        view.owner = player;
        view.faction = pawn.faction;
        view.yaw = pawn.yaw;
        view.maxHealth = spec->health;
        view.health = view.maxHealth;
        view.life = spec->lifeSec;
        view.pos[0] = pos[0];
        view.pos[1] = pos[1];
        view.pos[2] = pos[2];

        Rec rec{};
        rec.local = CreateDeployableEntity(view);
        view.entity = (rec.local != 0) ? rec.local : g_nextSyntheticEntity++;
        rec.view = view;
        rec.nextShotAt = m_clock;
        rec.nextPulseAt = m_clock + kAmmoPackPulseSec;

        m_deployables[view.entity] = rec;
        if (view.kind == kDeployShieldWall)
            CreateWallBody(m_deployables[view.entity]); // authority path; Jolt null-checked inside
        // Re-acquire the index — ServerDestroyDeployable above may have erased
        // emptied kind/owner entries and invalidated earlier references.
        m_ownerIndex[player][static_cast<uint8_t>(kind)].push_back(view.entity);
        ++m_placed;

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            SendCreate(kInvalidPlayer, m_deployables[view.entity].view);
#endif

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] deployable %u (kind %u) placed by player %u at (%.0f %.0f %.0f)",
                       view.entity, static_cast<unsigned>(kind), player, view.pos[0], view.pos[1], view.pos[2]);

        // W8: server-side placement bus event (synchronous, authority-only by
        // construction -- this whole function early-outs on non-authority).
        // Fired LAST so subscribers observe the fully registered deployable
        // (m_deployables/m_ownerIndex updated, Create already replicated), and
        // so a re-entrant subscriber chain (e.g. TFDirectiveSystem tier payout
        // -> ServerAwardXP -> EvXPAwarded) runs after all state changes here.
        if (m_events)
            m_events->Fire(EvDeployablePlaced{view.entity, view.kind, view.owner, view.faction});

        return TFDeployResult::Ok;
    }

    // ---------------------------------------------------------------------------
    // Server tick
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::ServerTick(float dt)
    {
#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            ServerPollNewClients();
        else
            m_knownClients.clear();
#endif

        // Lifetime + mechanics. Ticks never insert/erase on this map (pawn damage,
        // value-only repairs, and DEFERRED vehicle shots), so iteration is safe;
        // expiries are collected and destroyed afterwards.
        std::vector<EntityId> expired;
        for (auto& [entity, rec] : m_deployables)
        {
            rec.view.life -= dt;
            if (rec.view.life <= 0.0f)
            {
                expired.push_back(entity);
                continue;
            }
            // Extended W6 kinds are constexpr values past the frozen enum, so they
            // dispatch by comparison (a case label past COUNT would trip C4063).
            if (rec.view.kind == kDeployResupplyStation)
            {
                TickResupplyStation(rec);
            }
            else if (rec.view.kind == kDeployAVTurret)
            {
                TickAVTurret(rec); // queues into m_pendingAvShots (applied below)
            }
            else // ShieldWall is passive; W3 kinds keep their switch
            {
                switch (rec.view.kind)
                {
                case DeployableKind::FabTurret:
                    TickTurret(rec);
                    break;
                case DeployableKind::FabAmmoPack:
                    TickAmmoPack(rec);
                    break;
                case DeployableKind::MedBeacon:
                    TickMedBeacon(rec, dt);
                    break;
                default:
                    break;
                }
            }
        }
        for (EntityId e : expired)
        {
            ++m_expired;
            ServerDestroyDeployable(e, "expired");
        }

        // AV turret shots are applied AFTER the iteration: a destroyed vehicle's
        // explosion chain (TFWeaponServer::ExplodeAt -> splash vs deployables) may
        // erase from m_deployables, which must never happen mid-loop above.
        if (!m_pendingAvShots.empty())
        {
            if (m_ctx->vehicles)
                for (const PendingAVShot& s : m_pendingAvShots)
                    m_ctx->vehicles->ServerDamageVehicle(s.vehicle, kAVTurretDamage, s.turretEntity, s.owner,
                                                         m_avWeapon);
            m_pendingAvShots.clear();
        }

#ifdef ENABLE_NETWORKING
        // Slow keepalive so client health/life mirrors cannot drift for long.
        m_keepaliveAccum += dt;
        if (m_keepaliveAccum >= kKeepaliveSec)
        {
            m_keepaliveAccum = 0.0f;
            if (ServerNetActive())
                for (const auto& [entity, rec] : m_deployables)
                    SendUpdate(rec.view);
        }
#endif
    }

    bool TFDeployableSystem::TurretHasLoS(const float from[3], const float to[3]) const
    {
        if (!m_ctx || !m_ctx->world)
            return true;
        // 1 m terrain sampling, same approach as TFWeaponServer::TerrainBlocked.
        const float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len <= 1.0f)
            return true;
        const float inv = 1.0f / len;
        for (float t = 1.0f; t < len; t += 1.0f)
        {
            const float x = from[0] + d[0] * inv * t;
            const float y = from[1] + d[1] * inv * t;
            const float z = from[2] + d[2] * inv * t;
            if (y < m_ctx->world->TerrainHeightAt(x, z))
                return false;
        }
        return true;
    }

    void TFDeployableSystem::TickTurret(Rec& rec)
    {
        if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextShotAt)
            return;

        const float muzzle[3] = {rec.view.pos[0], rec.view.pos[1] + kTurretMuzzleY, rec.view.pos[2]};

        // (Re)acquire: nearest alive enemy pawn in range with terrain LoS. The
        // current target is naturally revalidated because it competes as nearest.
        const float range2 = kTurretRangeM * kTurretRangeM;
        EntityId best = 0;
        float bestD2 = range2;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction == rec.view.faction || p.faction == FactionId::None)
                    return;
                const float aim[3] = {p.pos[0], p.pos[1] + kTurretAimY, p.pos[2]};
                const float d2 = Dist2(rec.view.pos, p.pos);
                if (d2 > bestD2 || !TurretHasLoS(muzzle, aim))
                    return;
                best = p.entity;
                bestD2 = d2;
            });
        rec.targetPawn = best;
        if (best == 0)
            return;

        // Lazy killfeed weapon id: dedicated "fab_turret" row if the data agent
        // adds one; falls back to "-" in the killfeed (TF-W4: own weapon row).
        if (m_turretWeapon == kInvalidWeapon && m_ctx->data && m_ctx->data->IsLoaded())
            if (const WeaponDef* wd = m_ctx->data->GetWeaponByKey("fab_turret"))
                m_turretWeapon = wd->id;

        // Instant hitscan through the damage pipeline: OWNER gets kill credit +
        // hitmarker; the turret entity is the attacker pawn for direction UI.
        m_ctx->damage->ServerApplyDamage(best, rec.view.entity, rec.view.owner, kTurretDamage, kDamageKindBullet,
                                         m_turretWeapon, false);
        ++m_turretShots;
        rec.nextShotAt = m_clock + 60.0 / kTurretRpm; // tracers/audio are TF-W4
    }

    void TFDeployableSystem::TickAmmoPack(Rec& rec)
    {
        if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextPulseAt)
            return;
        rec.nextPulseAt = m_clock + kAmmoPackPulseSec;

        // W3 stand-in effect: small heal pulse to same-faction pawns in 5 m.
        // TF-W4: real mag/reserve refill (server ShooterState + client ammo sync).
        const float r2 = kAmmoPackRadiusM * kAmmoPackRadiusM;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
                    return;
                m_ctx->damage->ServerHeal(p.entity, kAmmoPackHealHp);
                m_healGiven += kAmmoPackHealHp;
            });
    }

    void TFDeployableSystem::TickMedBeacon(Rec& rec, float dt)
    {
        if (!m_ctx->players || !m_ctx->damage)
            return;
        const float r2 = kBeaconRadiusM * kBeaconRadiusM;
        const float amount = kBeaconHealPerSec * dt;
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
                    return;
                m_ctx->damage->ServerHeal(p.entity, amount); // clamps, never revives
                m_healGiven += amount;
            });
    }

    void TFDeployableSystem::TickResupplyStation(Rec& rec)
    {
        if (!m_ctx->players || !m_ctx->damage || m_clock < rec.nextPulseAt)
            return;
        rec.nextPulseAt = m_clock + kResupplyPulseSec;
        const float r2 = kResupplyRadiusM * kResupplyRadiusM;

        // Infantry: heal pulse (same TF-W4 real-ammo-refill caveat as FabAmmoPack).
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& p)
            {
                if (p.faction != rec.view.faction || Dist2(rec.view.pos, p.pos) > r2)
                    return;
                m_ctx->damage->ServerHeal(p.entity, kResupplyHealHp);
                m_healGiven += kResupplyHealHp;
            });

        // Field repair: friendly deployables in radius. Only VALUES of other
        // records change (no insert/erase), so this nested walk is safe inside
        // ServerTick's iteration.
        for (auto& [otherId, other] : m_deployables)
        {
            if (otherId == rec.view.entity || other.view.faction != rec.view.faction)
                continue;
            if (other.view.health >= other.view.maxHealth || Dist2(rec.view.pos, other.view.pos) > r2)
                continue;
            const float before = other.view.health;
            other.view.health = std::min(other.view.maxHealth, other.view.health + kResupplyRepairHp);
            m_repairGiven += other.view.health - before;
#ifdef ENABLE_NETWORKING
            if (ServerNetActive())
                SendUpdate(other.view);
#endif
        }
    }

    void TFDeployableSystem::TickAVTurret(Rec& rec)
    {
        if (!m_ctx->vehicles || m_clock < rec.nextShotAt)
            return;

        const float muzzle[3] = {rec.view.pos[0], rec.view.pos[1] + kAVTurretMuzzleY, rec.view.pos[2]};

        // Nearest alive ENEMY vehicle in range with terrain LoS (same acquire
        // shape as the infantry turret; vehicles only — never pawns).
        const float range2 = kAVTurretRangeM * kAVTurretRangeM;
        EntityId best = 0;
        float bestD2 = range2;
        m_ctx->vehicles->ForEachVehicle(
            [&](const TFVehicleInfo& v)
            {
                if (v.faction == rec.view.faction || v.faction == FactionId::None || v.hp <= 0.0f)
                    return;
                const float aim[3] = {v.pos[0], v.pos[1] + kAVTurretAimY, v.pos[2]};
                const float d2 = Dist2(rec.view.pos, v.pos);
                if (d2 > bestD2 || !TurretHasLoS(muzzle, aim))
                    return;
                best = v.entity;
                bestD2 = d2;
            });
        rec.targetPawn = best; // target slot reused for the debug panel
        if (best == 0)
            return;

        // Lazy killfeed weapon id: dedicated "av_turret" row if the data lane adds
        // one; falls back to "-" in the killfeed like the infantry turret.
        if (m_avWeapon == kInvalidWeapon && m_ctx->data && m_ctx->data->IsLoaded())
            if (const WeaponDef* wd = m_ctx->data->GetWeaponByKey("av_turret"))
                m_avWeapon = wd->id;

        // Deferred to after ServerTick's loop — see the apply site for why.
        m_pendingAvShots.push_back({best, rec.view.entity, rec.view.owner});
        ++m_avShots;
        rec.nextShotAt = m_clock + kAVTurretShotSec;
    }

    // ---------------------------------------------------------------------------
    // Damage / destroy (server)
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::ServerDamageDeployable(EntityId deployable, PlayerId attackerPlayer, float amount)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
            return;
        auto it = m_deployables.find(deployable);
        if (it == m_deployables.end())
            return;
        Rec& rec = it->second;

        // Same-faction fire is ignored (cheap objects; W4 revisits grief policy).
        if (m_ctx->players && attackerPlayer != kInvalidPlayer &&
            m_ctx->players->FactionOf(attackerPlayer) == rec.view.faction)
            return;

        rec.view.health -= amount;
        if (rec.view.health > 0.0f)
        {
#ifdef ENABLE_NETWORKING
            if (ServerNetActive())
                SendUpdate(rec.view);
#endif
            return;
        }
        ++m_destroyedByDamage;
        ServerDestroyDeployable(deployable, "destroyed");
    }

    void TFDeployableSystem::ServerSplashDamageDeployables(const float at[3], float radiusM, float damage,
                                                           PlayerId attackerPlayer)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || radiusM <= 0.0f || damage <= 0.0f)
            return;

        // Collect first — ServerDamageDeployable may erase entries.
        std::vector<std::pair<EntityId, float>> hits;
        for (const auto& [entity, rec] : m_deployables)
        {
            const float d = std::sqrt(Dist2(rec.view.pos, at));
            if (d > radiusM)
                continue;
            const float dmg = damage * (1.0f - d / radiusM); // pawn splash falloff
            if (dmg > 1.0f)
                hits.emplace_back(entity, dmg);
        }
        for (const auto& [entity, dmg] : hits)
            ServerDamageDeployable(entity, attackerPlayer, dmg);
    }

    void TFDeployableSystem::ServerDestroyDeployable(EntityId entity, const char* why)
    {
        auto it = m_deployables.find(entity);
        if (it == m_deployables.end())
            return;
        Rec& rec = it->second;

#ifdef ENABLE_NETWORKING
        if (ServerNetActive())
            SendDestroy(entity);
#endif

        auto oit = m_ownerIndex.find(rec.view.owner);
        if (oit != m_ownerIndex.end())
        {
            auto kit = oit->second.find(static_cast<uint8_t>(rec.view.kind));
            if (kit != oit->second.end())
            {
                auto& list = kit->second;
                list.erase(std::remove(list.begin(), list.end(), entity), list.end());
                if (list.empty())
                    oit->second.erase(kit);
            }
            if (oit->second.empty())
                m_ownerIndex.erase(oit);
        }

        ReleaseWallBody(rec);
        DestroyLocalEntity(rec.local);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] deployable %u removed (%s)", entity, why);
        m_deployables.erase(it);
    }

    // ---------------------------------------------------------------------------
    // Entities & visuals (both sides)
    // ---------------------------------------------------------------------------

    uint32_t TFDeployableSystem::CreateDeployableEntity(const TFDeployableView& view)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return 0;

        EntityID e = world->CreateEntity("TF_Deploy_K" + std::to_string(static_cast<unsigned>(view.kind)));
        if (static_cast<uint32_t>(e) == 0) // id 0 is reserved (same dodge as pawns)
            e = world->CreateEntity("TF_Deploy_K" + std::to_string(static_cast<unsigned>(view.kind)));

        Transform& t = world->AddComponent<Transform>(e);
        t.position = {view.pos[0], view.pos[1], view.pos[2]};
        t.rotation.y = view.yaw * kRadToDeg;

        NetworkIdentity& ni = world->AddComponent<NetworkIdentity>(e);
        ni.networkID = (view.entity != 0) ? view.entity : static_cast<uint32_t>(e);
        ni.ownerClientID = view.owner;
        ni.isLocalAuthority = m_ctx->IsAuthority();

        TFDeployableComp& dc = world->AddComponent<TFDeployableComp>(e);
        dc.kind = view.kind;
        dc.owner = view.owner;
        dc.life = view.life;

        world->AddComponent<TFFactionComp>(e).faction = view.faction;

        AttachDeployableVisual(static_cast<uint32_t>(e), view.kind, view.faction);
        return static_cast<uint32_t>(e);
    }

    void TFDeployableSystem::DestroyLocalEntity(uint32_t local)
    {
        if (local == 0)
            return;
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        const auto e = static_cast<EntityID>(local);
        if (world && world->GetRegistry().valid(e))
            world->DestroyEntity(e);
    }

    void TFDeployableSystem::AttachDeployableVisual(uint32_t local, DeployableKind kind, FactionId faction)
    {
        World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
        const auto e = static_cast<EntityID>(local);
        if (!world || !world->GetRegistry().valid(e) || world->HasComponent<MeshRenderer>(e))
            return;

        // Prop stand-ins (Assets/Models/MMOFPS/props, W1 asset staging): turret =
        // antenna mast, ammo pack = crate, med beacon = slim antenna (data-driven
        // via Assets/MMOFPS/Data/deployables.json). Faction is telegraphed by
        // material like pawn visuals; bespoke meshes are TF-W4. W6 kinds try their
        // own data row first, then fall back to the aliased W3 row scaled by the
        // spec's visualScaleMult (TFDeployableTypes.h).
        const DeployableVisualDef* dv = nullptr;
        bool aliased = false;
        if (m_ctx->data && m_ctx->data->IsLoaded())
        {
            dv = m_ctx->data->GetDeployableVisual(kind);
            if (!dv)
            {
                dv = m_ctx->data->GetDeployableVisual(TFDeployVisualAlias(kind));
                aliased = true;
            }
        }
        if (!dv)
            return; // no data table loaded (e.g. headless harness) - nothing to draw

        MeshRenderer& mr = world->AddComponent<MeshRenderer>(e);
        mr.meshPath = "Assets/" + dv->model;
        mr.materialPath = FactionStructureMaterial(*m_ctx, faction);
        mr.castShadows = true;

        // visualScaleMult only compensates the ALIASED base row; a bespoke data
        // row already carries its final scale (integration note: never stack both).
        const TFDeployableSpec* spec = TFDeploySpecOf(kind);
        const float* mult = (aliased && spec) ? spec->visualScaleMult : nullptr;
        if (Transform* t = world->GetComponent<Transform>(e))
            t->scale = {dv->scale[0] * (mult ? mult[0] : 1.0f), dv->scale[1] * (mult ? mult[1] : 1.0f),
                        dv->scale[2] * (mult ? mult[2] : 1.0f)};
    }

    // ---------------------------------------------------------------------------
    // Queries (both sides)
    // ---------------------------------------------------------------------------

    bool TFDeployableSystem::GetDeployable(EntityId entity, TFDeployableView& out) const
    {
        auto it = m_deployables.find(entity);
        if (it == m_deployables.end())
            return false;
        out = it->second.view;
        return true;
    }

    void TFDeployableSystem::ForEachDeployable(const std::function<void(const TFDeployableView&)>& fn) const
    {
        for (const auto& [entity, rec] : m_deployables)
            fn(rec.view);
    }

#ifdef ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Replication — server broadcast (patterns mirror TFReplication.cpp)
    // ---------------------------------------------------------------------------

    bool TFDeployableSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
    }

    bool TFDeployableSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Client;
    }

    void TFDeployableSystem::ServerPollNewClients()
    {
        // No join callback slot on NetworkManager — diff GetClients() like
        // TFServerSim/TFReplication so late joiners get the current set.
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const auto& clients = nm.GetClients();

        for (const auto& [id, info] : clients)
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            for (const auto& [entity, rec] : m_deployables)
                SendCreate(id, rec.view);
        }
        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            if (!clients.contains(*it))
                it = m_knownClients.erase(it);
            else
                ++it;
        }
    }

    void TFDeployableSystem::SendCreate(PlayerId target, const TFDeployableView& view)
    {
        TF_RepDeployCreate c{};
        c.entityId = view.entity;
        c.ownerPlayer = view.owner;
        c.kind = static_cast<uint8_t>(view.kind);
        c.faction = static_cast<uint8_t>(view.faction);
        c.posX = view.pos[0];
        c.posY = view.pos[1];
        c.posZ = view.pos[2];
        c.yaw = view.yaw;
        c.health = view.health;
        c.maxHealth = view.maxHealth;
        c.lifeSec = view.life;
        SendRep(target, kTFRepMsg_DeployCreate, &c, sizeof(c), true);
    }

    void TFDeployableSystem::SendUpdate(const TFDeployableView& view)
    {
        TF_RepDeployUpdate u{};
        u.entityId = view.entity;
        u.health = view.health;
        u.lifeSec = view.life;
        SendRep(kInvalidPlayer, kTFRepMsg_DeployUpdate, &u, sizeof(u), false);
    }

    void TFDeployableSystem::SendDestroy(EntityId entity)
    {
        TF_RepDeployDestroy d{entity};
        SendRep(kInvalidPlayer, kTFRepMsg_DeployDestroy, &d, sizeof(d), true);
    }

    void TFDeployableSystem::SendRep(PlayerId target, uint16_t msgId, const void* payload, size_t size, bool reliable)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = reliable ? Spark::Net::ChannelType::Reliable : Spark::Net::ChannelType::Unreliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        if (target == kInvalidPlayer)
            nm.SendToAll(msg);
        else
            nm.SendToClient(target, msg);
    }

    // ---------------------------------------------------------------------------
    // Replication — client mirror
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::ClientEnsureHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployCreate),
                           [this](const NetworkMessage& m) { OnDeployCreate(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployUpdate),
                           [this](const NetworkMessage& m) { OnDeployUpdate(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFRepMsg_DeployDestroy),
                           [this](const NetworkMessage& m) { OnDeployDestroy(m.payload.data(), m.payload.size()); });
        m_clientHandlers = true;
    }

    void TFDeployableSystem::ClientReleaseHandlers()
    {
        // No per-type removal on NetworkManager — swap in no-ops so no dangling
        // `this` survives shutdown (established TFReplication pattern).
        using Spark::Net::MessageType;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFRepMsg_DeployCreate, kTFRepMsg_DeployUpdate, kTFRepMsg_DeployDestroy})
            nm.RegisterHandler(static_cast<MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFDeployableSystem::OnDeployCreate(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepDeployCreate))
            return;
        TF_RepDeployCreate c;
        std::memcpy(&c, data, sizeof(c));
        if (c.kind >= kTFDeployKindCount)
            return; // stale/newer server sent a kind this build doesn't know

        Rec& rec = m_deployables[c.entityId]; // upsert (re-create keeps the visual)
        rec.view.entity = c.entityId;
        rec.view.kind = static_cast<DeployableKind>(c.kind);
        rec.view.owner = c.ownerPlayer;
        rec.view.faction = static_cast<FactionId>(c.faction);
        rec.view.pos[0] = c.posX;
        rec.view.pos[1] = c.posY;
        rec.view.pos[2] = c.posZ;
        rec.view.yaw = c.yaw;
        rec.view.health = c.health;
        rec.view.maxHealth = c.maxHealth;
        rec.view.life = c.lifeSec;
        if (rec.local == 0)
            rec.local = CreateDeployableEntity(rec.view);
    }

    void TFDeployableSystem::OnDeployUpdate(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepDeployUpdate))
            return;
        TF_RepDeployUpdate u;
        std::memcpy(&u, data, sizeof(u));
        auto it = m_deployables.find(u.entityId);
        if (it == m_deployables.end())
            return; // unreliable update raced the reliable Create — skip
        it->second.view.health = u.health;
        it->second.view.life = u.lifeSec;
    }

    void TFDeployableSystem::OnDeployDestroy(const void* data, size_t size)
    {
        if (size != sizeof(TF_RepDeployDestroy))
            return;
        TF_RepDeployDestroy d;
        std::memcpy(&d, data, sizeof(d));
        auto it = m_deployables.find(d.entityId);
        if (it == m_deployables.end())
            return;
        DestroyLocalEntity(it->second.local);
        m_deployables.erase(it);
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFDeployableSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Deployables"))
            return;
        ImGui::Text("live: %zu  placed: %u  refused: %u  expired: %u  killed: %u", m_deployables.size(), m_placed,
                    m_refusedPlacements, m_expired, m_destroyedByDamage);
        ImGui::Text("turret shots: %u  AV shots: %u  healed: %.0f hp  repaired: %.0f hp", m_turretShots, m_avShots,
                    m_healGiven, m_repairGiven);
        for (const auto& [entity, rec] : m_deployables)
        {
            const TFDeployableSpec* sp = TFDeploySpecOf(rec.view.kind);
            ImGui::Text("e%u %s p%u %s hp=%.0f/%.0f life=%.0fs pos=(%.0f %.0f %.0f)", entity, sp ? sp->name : "?",
                        rec.view.owner, FactionTag(rec.view.faction), rec.view.health, rec.view.maxHealth,
                        rec.view.life, rec.view.pos[0], rec.view.pos[1], rec.view.pos[2]);
        }
#endif
    }

} // namespace Terrafront
