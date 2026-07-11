/**
 * @file TFWeaponServer.cpp
 * @brief Server half of TERRAFRONT weapons: fire validation (RoF token
 *        bucket), lag-compensated hitscan with pellet spread, server-side
 *        projectile simulation with splash, terrain occlusion, and
 *        physics-backed ballistics (world occlusion, hit zones, splash LOS)
 *        via Game/TFBallistics.h. Client half in TFWeaponSystem.cpp.
 */
#include "Game/TFWeaponSystem.h"

#include "Game/TFAbilitySystem.h" // class-abilities lane (W9): Lockdown RoF bonus
#include "Game/TFBallistics.h"
#include "Game/TFDamageSystem.h"
#include "Game/TFDeployableSystem.h" // W3 shared-edit: splash vs deployables
#include "Game/TFImpactFx.h"         // W11 impact-broadcast: in-process listen-host route
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h" // W6 progression: per-weapon shot/hit stats
#include "Game/TFVehicleSystem.h"     // W3 shared-edit: vehicle hit tests + seat weapons
#include "Game/TFWeaponMath.h"
#include "Net/TFFireFxProtocol.h" // W9 remote-fire-events: 0x54F4 S->C fx broadcast
#include "Net/TFServerSim.h"
#include "World/TFWorldSetup.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Terrafront
{

    namespace
    {
        constexpr float kMaxHitscanRangeM = 400.0f;
        constexpr float kProjectileLifeSec = 8.0f;
        constexpr double kFixedRttSec = 0.100; // TF-W2: per-client RTT from NetworkManager
        constexpr uint8_t kDamageKindBullet = 0;
        constexpr uint8_t kDamageKindExplosive = 1;
        // Projectile gravity + pawn capsule bands now live in Game/TFBallistics.h
        // (Ballistics::kProjectileGravityMps2, WeaponMath::kPawnHeightM/kHeadZoneM).
    } // namespace

    double TFWeaponSystem::ServerNow() const
    {
        return (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_serverClock;
    }

    // SECURITY: mirrors the client's RefreshLocalLoadout() slot resolution
    // (primary/secondary/tool/melee) but is evaluated server-side against the
    // server-trusted pawn.cls/pawn.faction, so a client cannot claim a weapon
    // outside what its class is actually issued. Checks every entry in
    // primarySlots (not just front()) so a future multi-primary class stays
    // correct without another server-side change.
    bool TFWeaponSystem::IsWeaponInLoadout(WeaponId fireWeapon, const PawnInfo& pawn) const
    {
        if (!m_ctx || !m_ctx->data)
            return false;

        const ClassDef* cls = m_ctx->data->GetClass(pawn.cls);
        if (!cls)
            return false;

        for (const std::string& slotKey : cls->primarySlots)
        {
            if (FindWeaponForSlotKey(slotKey, pawn.faction) == fireWeapon)
                return true;
        }
        if (FindWeaponForSlotKey(cls->secondarySlot, pawn.faction) == fireWeapon)
            return true;
        if (FindToolWeapon(cls->toolKey) == fireWeapon)
            return true;
        if (FindWeaponForSlotKey("melee", pawn.faction) == fireWeapon)
            return true;
        return false;
    }

    // ---------------------------------------------------------------------------
    // Frozen API: server fire entry
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::ServerHandleFire(PlayerId shooter, const TF_FireEvent& ev)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players || !m_ctx->data ||
            !m_ctx->data->IsLoaded())
            return;

        PawnInfo pawn;
        if (!m_ctx->players->GetPawnByPlayer(shooter, pawn) || !pawn.alive)
            return;

        // W3 shared-edit (vehicles agent): a seated shooter fires the SEAT weapon
        // through this exact validation path — the riding pawn is the shooter
        // surrogate (its transform already sits on the vehicle, so the trusted
        // origin, lag comp and hit confirms all work unchanged). Unarmed seats
        // (passengers, drivers of unarmed vehicles) cannot fire at all.
        WeaponId fireWeapon = ev.weaponId;
        bool viaVehicleSeat = false;
        if (m_ctx->vehicles)
        {
            WeaponId seatWeapon = kInvalidWeapon;
            if (m_ctx->vehicles->GetSeatWeapon(shooter, seatWeapon))
            {
                if (seatWeapon == kInvalidWeapon)
                    return; // seated in an unarmed seat
                fireWeapon = seatWeapon;
                viaVehicleSeat = true;
            }
        }

        // SECURITY: an unseated shooter must be firing a weapon that's actually
        // part of their class loadout. Without this, a modified client can put
        // any WeaponId in TF_FireEvent (another class's weapon, a dev/OP-only
        // weapon, an out-of-range id) and have the server fire it with full
        // trust. Seated shooters are exempt: fireWeapon there is the
        // server-resolved seat weapon (GetSeatWeapon), not client input.
        if (!viaVehicleSeat && !IsWeaponInLoadout(fireWeapon, pawn))
            return;

        // W8 unlock enforcement (the deliberate non-wiring from W6): a
        // loadout-ELIGIBLE weapon must also be UNLOCKED for this player
        // (Persistence/TFUnlockTree.h via TFProgressionSystem). Weapons with no
        // tree entry are default kit and always pass -- IsWeaponUnlocked
        // returns true for them, and fails open while data tables are not
        // loaded (verified against TFUnlockTree::All(): only carbines, lmgs,
        // snipers, np_shotgun and np_launcher are gated). One deliberate
        // exemption: the class-DEFAULT primary (primarySlots.front()). Ghost's
        // default equip is a tree-gated sniper and Striker/Fabricator's is the
        // rank-3 carbine; RefreshLocalLoadout hands those to every fresh spawn
        // regardless of rank, so rejecting them would brick rank-1 players of
        // those classes. Every OTHER slot-pool weapon (e.g. np_shotgun on
        // Striker, the carbine in Ghost's second primary slot) is enforced.
        // Seated shooters stay exempt like the loadout check above: their
        // fireWeapon is the server-resolved seat weapon, not client input.
        if (!viaVehicleSeat && m_ctx->progression && !m_ctx->progression->IsWeaponUnlocked(shooter, fireWeapon))
        {
            const ClassDef* cls = m_ctx->data->GetClass(pawn.cls);
            const bool defaultPrimary = cls && !cls->primarySlots.empty() &&
                                        FindWeaponForSlotKey(cls->primarySlots.front(), pawn.faction) == fireWeapon;
            if (!defaultPrimary)
            {
                ++m_shotsRejected;
                return;
            }
        }

        const WeaponDef* base = m_ctx->data->GetWeapon(fireWeapon);
        if (!base || base->kind == "melee" || base->kind == "beam")
            return; // TF-W2: melee reach + tool beams take a different server path

        WeaponDef def = m_ctx->data->ResolveWeapon(fireWeapon, pawn.faction);
        // class-abilities lane (W9): Anchor Lockdown RoF bonus — MUST land
        // together with the client fire-interval edit (both-or-neither).
        if (m_ctx->abilities)
            def.rofRpm *= m_ctx->abilities->RoFMultiplierForPawn(pawn.entity);

        ShooterState& st = m_shooters[shooter];
        const double now = ServerNow();
        if (!ValidateFire(def, st, now))
        {
            ++m_shotsRejected;
            return;
        }
        ++m_shotsValidated;

        // W8 audio-polish: a validated fire event is the local client's only
        // in-process knowledge of OTHER shooters (bots + remote players on a
        // listen host), so the distant-gunfire layer hooks here. No-op on
        // dedicated servers and for the local shooter (checked inside).
        ClientOnRemoteFire(shooter, pawn, def);

        // W6 progression: one validated trigger pull == one shot (pellets are
        // still one shot; hits below are capped at one per fire event to match).
        if (m_ctx->progression)
            m_ctx->progression->ServerRecordShots(shooter, def.id, 1);

        // Server-trusted origin: the pawn's eye. Client dir is used but normalized.
        float origin[3] = {pawn.pos[0], pawn.pos[1] + 1.65f, pawn.pos[2]};
        // Registry-stub pawns report zeros; fall back to the client origin then.
        if (pawn.pos[0] == 0.0f && pawn.pos[1] == 0.0f && pawn.pos[2] == 0.0f)
        {
            origin[0] = ev.originX;
            origin[1] = ev.originY;
            origin[2] = ev.originZ;
        }
        float dir[3] = {ev.dirX, ev.dirY, ev.dirZ};
        if (!WeaponMath::Normalize3(dir))
            return;

        // W8 shared-edit (turret-aim/vehicles agent): the turret-controller seat
        // fires from the AIMED turret muzzle frame (server aim state, unit dir)
        // instead of the riding pawn's eye, so shots match the replicated turret
        // visual. Non-controller armed seats keep the pawn-eye path above.
        if (viaVehicleSeat)
            m_ctx->vehicles->GetSeatFireFrame(shooter, origin, dir);

        // W9 remote-fire-events: PURE clients have no in-process view of this
        // validated fire (ClientOnRemoteFire above only covers the listen
        // host), so broadcast a per-shot fx message from the FINAL fire frame
        // (post seat/turret resolution) to clients in range. Both hitscan and
        // projectile paths pass through here.
        ServerBroadcastRemoteFireFx(st, shooter, pawn.entity, def.id, origin, dir);

        if (def.projSpeed > 0.0f)
        {
            SpawnServerProjectile(shooter, pawn, def, origin, dir);
            return;
        }

        const double rewindTime = now - kFixedRttSec * 0.5;
        const int pellets = std::max(1, def.pellets);
        bool anyPelletHit = false;
        for (int i = 0; i < pellets; ++i)
        {
            float pdir[3] = {dir[0], dir[1], dir[2]};
            if (pellets > 1 || def.spreadHipDeg > 0.0f)
                WeaponMath::PerturbCone(pdir, def.spreadHipDeg * 0.5f, m_rng);
            if (FireHitscanRay(shooter, pawn, def, origin, pdir, kMaxHitscanRangeM, rewindTime, kDamageKindBullet))
                anyPelletHit = true;
        }
        // W6 progression: at most one hit per fire event (multi-pellet shotguns
        // count as a single hit so accuracy never exceeds 100%).
        if (anyPelletHit && m_ctx->progression)
            m_ctx->progression->ServerRecordHits(shooter, def.id, 1);
    }

    // ---------------------------------------------------------------------------
    // W9 remote-fire-events: 0x54F4 fx broadcast (Net/TFFireFxProtocol.h)
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::ServerBroadcastRemoteFireFx(ShooterState& st, PlayerId shooter, EntityId shooterPawn,
                                                     WeaponId weapon, const float muzzle[3], const float dir[3])
    {
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return; // Standalone / client role: no remote clients can exist
        const auto& clients = nm.GetClients();
        if (clients.empty())
            return;

        // Per-shooter rate cap (~10/s). The stamp only advances when a message
        // is actually delivered below, so shooters with nobody in range never
        // burn their budget ("skip if no clients in range").
        const double now = ServerNow();
        if (now - st.lastFireFxSent < kTFRemoteFireFxMinIntervalSec)
            return;

        const TF_RemoteFireFx fx = TF_RemoteFireFx::From(shooterPawn, weapon, muzzle, dir);
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFFxMsg_RemoteFire);
        msg.channel = Spark::Net::ChannelType::Unreliable; // presentational; drops are free
        msg.payload.resize(sizeof(fx));
        std::memcpy(msg.payload.data(), &fx, sizeof(fx));

        constexpr float kRange2 = kTFRemoteFireFxRangeM * kTFRemoteFireFxRangeM;
        bool sentAny = false;
        for (const auto& [clientId, info] : clients)
        {
            if (clientId == shooter || info.state != Spark::Net::ConnectionState::Connected)
                continue; // never echo the shooter's own shot back
            PawnInfo listener;
            if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(clientId, listener))
                continue; // not entered world / no pawn yet — nothing to hear with
            if (WeaponMath::Dist2(listener.pos, muzzle) > kRange2)
                continue;
            nm.SendToClient(clientId, msg);
            sentAny = true;
        }
        if (sentAny)
            st.lastFireFxSent = now;
#else
        (void)st;
        (void)shooter;
        (void)shooterPawn;
        (void)weapon;
        (void)muzzle;
        (void)dir;
#endif
    }

    // ---------------------------------------------------------------------------
    // W11 impact-broadcast: 0x54F5 authoritative impact fx (Net/TFFireFxProtocol.h)
    // ---------------------------------------------------------------------------

    bool TFWeaponSystem::ImpactFxCapOpen(PlayerId shooter) const
    {
        const auto it = m_shooters.find(shooter);
        if (it == m_shooters.end())
            return true;
        return ServerNow() - it->second.lastImpactFxSent >= kTFImpactFxMinIntervalSec;
    }

    float TFWeaponSystem::TerrainHitT(const float origin[3], const float dir[3], float dist) const
    {
        // Same 1 m march as TerrainBlocked, plus the bisection refine the client
        // guess-trace used (TFImpactFx precedent) so the puff sits ON the
        // surface instead of up to 1 m past it.
        if (!m_ctx || !m_ctx->world)
            return -1.0f;
        const float step = 1.0f;
        for (float t = step; t < dist; t += step)
        {
            const float y = origin[1] + dir[1] * t;
            if (y < m_ctx->world->TerrainHeightAt(origin[0] + dir[0] * t, origin[2] + dir[2] * t))
            {
                float lo = t - step;
                float hi = t;
                for (int i = 0; i < 5; ++i)
                {
                    const float mid = 0.5f * (lo + hi);
                    const float my = origin[1] + dir[1] * mid;
                    if (my < m_ctx->world->TerrainHeightAt(origin[0] + dir[0] * mid, origin[2] + dir[2] * mid))
                        hi = mid;
                    else
                        lo = mid;
                }
                return 0.5f * (lo + hi);
            }
        }
        return -1.0f;
    }

    void TFWeaponSystem::ServerBroadcastImpactFx(PlayerId shooter, const float point[3], uint8_t surface)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;

        // Per-shooter rate cap (~8/s; multi-pellet shotguns collapse to one
        // puff per window). The stamp only advances when something was actually
        // delivered below — same semantics as the 0x54F4 fire-fx cap.
        ShooterState& st = m_shooters[shooter];
        const double now = ServerNow();
        if (now - st.lastImpactFxSent < kTFImpactFxMinIntervalSec)
            return;

        bool delivered = false;

        // In-process route: the listen host / standalone player never receives
        // 0x54F5 (a server cannot message itself), so bots' and remote players'
        // impacts near the host feed TFImpactFx directly. The local shooter is
        // excluded — their puff is the immediate OnLocalShot prediction.
        if (m_ctx->HasLocalPlayer() && m_ctx->localPlayer != shooter)
            delivered |= TFImpactFx::Get().OnServerImpact(*m_ctx, point, surface);

#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && !nm.GetClients().empty())
        {
            const TF_ImpactFx fx = TF_ImpactFx::From(point, static_cast<TFImpactSurface>(surface));
            Spark::Net::NetworkMessage msg;
            msg.type = static_cast<Spark::Net::MessageType>(kTFFxMsg_ImpactFx);
            msg.channel = Spark::Net::ChannelType::Unreliable; // presentational; drops are free
            msg.payload.resize(sizeof(fx));
            std::memcpy(msg.payload.data(), &fx, sizeof(fx));

            constexpr float kRange2 = kTFImpactFxRangeM * kTFImpactFxRangeM;
            for (const auto& [clientId, info] : nm.GetClients())
            {
                if (clientId == shooter || info.state != Spark::Net::ConnectionState::Connected)
                    continue; // the shooter's own puff is client-predicted
                PawnInfo listener;
                if (!m_ctx->players || !m_ctx->players->GetPawnByPlayer(clientId, listener))
                    continue; // not entered world / no pawn yet — nothing to see with
                if (WeaponMath::Dist2(listener.pos, point) > kRange2)
                    continue;
                nm.SendToClient(clientId, msg);
                delivered = true;
            }
        }
#endif
        if (delivered)
            st.lastImpactFxSent = now;
    }

    // ---------------------------------------------------------------------------
    // Validation
    // ---------------------------------------------------------------------------

    bool TFWeaponSystem::ValidateFire(const WeaponDef& def, ShooterState& st, double now)
    {
        const double perSec = std::max(1.0, static_cast<double>(def.rofRpm) / 60.0);

        // SECURITY: the RoF token bucket + approx mag live PER WEAPON ID here
        // (try_emplace only seeds a fresh bucket the FIRST time this shooter is
        // ever seen firing this weapon id). Previously this state was a single
        // slot keyed off "last weapon fired", reset to a full 2.0 tokens every
        // time the weapon id changed -- alternating between two weapon ids each
        // shot refilled the bucket every shot for unlimited fire rate. Persisting
        // per weapon id closes that: switching back to a weapon reuses its own
        // gradually-refilled bucket, never a fresh one.
        auto [it, inserted] = st.perWeapon.try_emplace(def.id);
        WeaponFireState& ws = it->second;
        if (inserted)
        {
            ws.lastRefill = now;
            ws.mag = def.magSize;
        }

        ws.tokens = std::min(2.0f, ws.tokens + static_cast<float>((now - ws.lastRefill) * perSec));
        ws.lastRefill = now;

        if (ws.tokens < 1.0f)
            return false;
        ws.tokens -= 1.0f;

        // Approximate server mag: refills after reloadSec of silence.
        if (ws.mag <= 0)
        {
            if (now - ws.magEmptyTime < def.reloadSec)
                return false;
            ws.mag = def.magSize;
        }
        if (--ws.mag <= 0)
            ws.magEmptyTime = now;

        ws.lastShotTime = now;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Hitscan
    // ---------------------------------------------------------------------------

    bool TFWeaponSystem::FireHitscanRay(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def,
                                        const float origin[3], const float dir[3], float maxDist, double rewindTime,
                                        uint8_t damageKind)
    {
#ifdef ENABLE_NETWORKING
        if (!m_ctx->serverSim || !m_ctx->damage)
            return false;

        // Ballistics: physics raycast against the world (buildings, rocks,
        // deployable shields). A hit body that carries an ECS pawn entity doubles
        // as hit registration; any other solid body clamps the ray. When physics
        // is unavailable (null GetPhysics / Jolt stub) blockDist stays maxDist and
        // the terrain heightfield below remains the only world occluder — exactly
        // the pre-ballistics behaviour.
        float blockDist = maxDist;
        EntityId physPawn = 0;
        float physPawnDist = 0.0f;
        float physPawnPoint[3]{};
        // W11 impact-broadcast: remember the world-geometry stop so blocked
        // shots can broadcast their true visual endpoint (0x54F5).
        bool wallValid = false;
        float wallPointFx[3]{};
        uint8_t wallSurface = static_cast<uint8_t>(TFImpactSurface::Static);
        Ballistics::WorldHit wall;
        if (Ballistics::RaycastWorld(m_ctx->engine, origin, dir, maxDist, wall) && wall.blocked)
        {
            PawnInfo pv;
            if (static_cast<EntityId>(wall.entityId) == pawn.entity || wall.dist < Ballistics::kMuzzleClearanceM)
            {
                // Shooter's own body / near-clip geometry (e.g. the hull of the
                // vehicle a gunner sits in): never eats the shot.
            }
            else if (wall.entityId != 0 && m_ctx->players &&
                     m_ctx->players->GetPawnByEntity(static_cast<EntityId>(wall.entityId), pv) && pv.alive)
            {
                physPawn = static_cast<EntityId>(wall.entityId);
                physPawnDist = wall.dist;
                physPawnPoint[0] = wall.point[0];
                physPawnPoint[1] = wall.point[1];
                physPawnPoint[2] = wall.point[2];
            }
            else
            {
                blockDist = wall.dist;
                wallValid = true;
                wallPointFx[0] = wall.point[0];
                wallPointFx[1] = wall.point[1];
                wallPointFx[2] = wall.point[2];
                TFDeployableView dv;
                if (wall.entityId != 0 && m_ctx->deployables &&
                    m_ctx->deployables->GetDeployable(static_cast<EntityId>(wall.entityId), dv))
                    wallSurface = static_cast<uint8_t>(TFImpactSurface::Shield);
            }
        }

        // W11 impact-broadcast: terrain-stop fx helper (cap pre-gated so capped
        // shots skip the march entirely).
        const auto fxTerrain = [&](float dist)
        {
            if (!ImpactFxCapOpen(shooter))
                return;
            const float tT = TerrainHitT(origin, dir, dist);
            if (tT <= 0.0f)
                return;
            const float pt[3] = {origin[0] + dir[0] * tT, origin[1] + dir[1] * tT, origin[2] + dir[2] * tT};
            ServerBroadcastImpactFx(shooter, pt, static_cast<uint8_t>(TFImpactSurface::Terrain));
        };

        float hitPoint[3];
        float hitDist = 0.0f;
        EntityId hit = m_ctx->serverSim->LagComp().RewindRaycast(rewindTime, origin, dir, maxDist, pawn.entity,
                                                                 hitPoint, &hitDist);

        // Physics pawn body as fallback registration when the lag-comp capsule set
        // missed (e.g. a pawn body not present in the rewind buffer). Rewound
        // capsules stay authoritative when they DO report a hit.
        if (hit == 0 && physPawn != 0)
        {
            hit = physPawn;
            hitDist = physPawnDist;
            hitPoint[0] = physPawnPoint[0];
            hitPoint[1] = physPawnPoint[1];
            hitPoint[2] = physPawnPoint[2];
        }

        // W3 shared-edit (vehicles agent): the same ray also tests vehicle hulls;
        // the NEAREST of pawn/vehicle wins. Vehicle hits take def.vsVehicleMult
        // and route to TFVehicleSystem's hp pool instead of ctx.damage. (Vehicles
        // move slowly relative to the lag-comp window, so present-time hulls are
        // an acceptable W3 approximation vs. rewound pawn capsules.)
        float vehPoint[3];
        float vehDist = 0.0f;
        const EntityId vehHit =
            m_ctx->vehicles ? m_ctx->vehicles->RaycastVehicles(origin, dir, maxDist, vehPoint, &vehDist) : 0;

        if (vehHit != 0 && (hit == 0 || vehDist < hitDist))
        {
            if (vehDist > blockDist)
            {
                if (wallValid)
                    ServerBroadcastImpactFx(shooter, wallPointFx, wallSurface);
                return false; // world geometry in front of the vehicle
            }
            if (TerrainBlocked(origin, dir, vehDist))
            {
                fxTerrain(vehDist);
                return false;
            }
            const float vdmg =
                WeaponMath::LinearFalloff(def.damage, def.minDamage, def.falloffStartM, def.falloffEndM, vehDist) *
                def.vsVehicleMult;
            m_ctx->vehicles->ServerDamageVehicle(vehHit, vdmg, pawn.entity, shooter, def.id);
            ServerBroadcastImpactFx(shooter, vehPoint, static_cast<uint8_t>(TFImpactSurface::Vehicle));
            return true;
        }

        if (hit == 0)
        {
            // W11 impact-broadcast: nothing registered — the shot's true visual
            // stop is the world-geometry block or terrain along the full ray
            // (a genuine sky shot broadcasts nothing).
            if (wallValid)
                ServerBroadcastImpactFx(shooter, wallPointFx, wallSurface);
            else
                fxTerrain(maxDist);
            return false;
        }

        if (hitDist > blockDist)
        {
            if (wallValid)
                ServerBroadcastImpactFx(shooter, wallPointFx, wallSurface);
            return false; // world geometry in front of the target
        }

        if (TerrainBlocked(origin, dir, hitDist))
        {
            fxTerrain(hitDist);
            return false;
        }

        // Hit zone from the hit height on the victim's capsule: head band takes
        // headshotMult, leg band takes the limb malus, torso is baseline.
        bool head = false;
        float zoneMult = 1.0f;
        PawnInfo victim;
        if (m_ctx->players->GetPawnByEntity(hit, victim) && victim.alive)
        {
            const Ballistics::HitZone zone = Ballistics::ClassifyHitZone(hitPoint, victim.pos);
            head = zone == Ballistics::HitZone::Head;
            zoneMult = Ballistics::ZoneDamageMult(zone, def.headshotMult);
        }

        const float damage =
            WeaponMath::LinearFalloff(def.damage, def.minDamage, def.falloffStartM, def.falloffEndM, hitDist) *
            zoneMult;

        m_ctx->damage->ServerApplyDamage(hit, pawn.entity, shooter, damage, damageKind, def.id, head);
        ServerBroadcastImpactFx(shooter, hitPoint, static_cast<uint8_t>(TFImpactSurface::Pawn));
        return true;
#else
        (void)shooter;
        (void)pawn;
        (void)def;
        (void)origin;
        (void)dir;
        (void)maxDist;
        (void)rewindTime;
        (void)damageKind;
        return false;
#endif
    }

    bool TFWeaponSystem::TerrainBlocked(const float origin[3], const float dir[3], float dist) const
    {
        if (!m_ctx || !m_ctx->world)
            return false;
        const float step = 1.0f;
        for (float t = step; t < dist; t += step)
        {
            const float x = origin[0] + dir[0] * t;
            const float y = origin[1] + dir[1] * t;
            const float z = origin[2] + dir[2] * t;
            if (y < m_ctx->world->TerrainHeightAt(x, z))
                return true;
        }
        return false;
    }

    EntityId TFWeaponSystem::RaycastPawnsNow(const float origin[3], const float dir[3], float maxDist, EntityId ignore,
                                             float outHitPoint[3], float* outDist) const
    {
#ifdef ENABLE_NETWORKING
        if (!m_ctx || !m_ctx->serverSim)
            return 0;
        return m_ctx->serverSim->LagComp().RewindRaycast(ServerNow(), origin, dir, maxDist, ignore, outHitPoint,
                                                         outDist);
#else
        (void)origin;
        (void)dir;
        (void)maxDist;
        (void)ignore;
        (void)outHitPoint;
        (void)outDist;
        return 0;
#endif
    }

    // ---------------------------------------------------------------------------
    // Projectiles
    // ---------------------------------------------------------------------------

    void TFWeaponSystem::SpawnServerProjectile(PlayerId shooter, const PawnInfo& pawn, const WeaponDef& def,
                                               const float origin[3], const float dir[3])
    {
        ServerProjectile p;
        p.shooter = shooter;
        p.shooterPawn = pawn.entity;
        p.weapon = def.id;
        p.pos[0] = origin[0];
        p.pos[1] = origin[1];
        p.pos[2] = origin[2];
        p.vel[0] = dir[0] * def.projSpeed;
        p.vel[1] = dir[1] * def.projSpeed;
        p.vel[2] = dir[2] * def.projSpeed;
        p.gravityFactor = def.gravity;
        p.damage = def.damage;
        p.minDamage = def.minDamage;
        p.falloffStartM = def.falloffStartM;
        p.falloffEndM = def.falloffEndM;
        p.headshotMult = def.headshotMult;
        p.splashRadiusM = def.splashRadiusM;
        p.splashDamage = def.splashDamage;
        m_projectiles.push_back(p);
    }

    void TFWeaponSystem::ServerStepProjectiles(float dt)
    {
        if (m_projectiles.empty())
            return;

        for (auto it = m_projectiles.begin(); it != m_projectiles.end();)
        {
            ServerProjectile& p = *it;
            p.lifeSec += dt;

            const float prev[3] = {p.pos[0], p.pos[1], p.pos[2]};
            Ballistics::IntegrateProjectile(p.pos, p.vel, p.gravityFactor, dt);

            float seg[3] = {p.pos[0] - prev[0], p.pos[1] - prev[1], p.pos[2] - prev[2]};
            const float segLen = WeaponMath::Len3(seg);
            p.traveledM += segLen;

            bool dead = false;

            // Pawn sweep along this step (lag-comp buffer holds current poses too).
            if (segLen > 1.0e-4f)
            {
                float dir[3] = {seg[0] / segLen, seg[1] / segLen, seg[2] / segLen};
                float hitPoint[3];
                float hitDist = 0.0f;
                EntityId hit = RaycastPawnsNow(prev, dir, segLen, p.shooterPawn, hitPoint, &hitDist);

                // Ballistics: physics sweep along this step — buildings/rocks/
                // deployable shields detonate the warhead mid-flight. A physics
                // body carrying a pawn entity doubles as hit registration when the
                // lag-comp sweep missed. Physics unavailable -> wallBlocked stays
                // false and the terrain check below is the only world impact.
                bool wallBlocked = false;
                float wallDist = segLen + 1.0f;
                float wallPoint[3]{};
                Ballistics::WorldHit wall;
                if (Ballistics::RaycastWorld(m_ctx->engine, prev, dir, segLen, wall) && wall.blocked)
                {
                    PawnInfo pv;
                    // Flight distance from the muzzle to this wall hit (traveledM
                    // already includes the current segment).
                    const float distFromMuzzle = p.traveledM - segLen + wall.dist;
                    if (static_cast<EntityId>(wall.entityId) == p.shooterPawn ||
                        distFromMuzzle < Ballistics::kMuzzleClearanceM)
                    {
                        // Shooter's own body / muzzle-adjacent geometry on the very
                        // first step (e.g. a gunner's own vehicle hull): ignore.
                    }
                    else if (wall.entityId != 0 && m_ctx->players &&
                             m_ctx->players->GetPawnByEntity(static_cast<EntityId>(wall.entityId), pv) && pv.alive)
                    {
                        if (hit == 0)
                        {
                            hit = static_cast<EntityId>(wall.entityId);
                            hitDist = wall.dist;
                            hitPoint[0] = wall.point[0];
                            hitPoint[1] = wall.point[1];
                            hitPoint[2] = wall.point[2];
                        }
                    }
                    else
                    {
                        wallBlocked = true;
                        wallDist = wall.dist;
                        wallPoint[0] = wall.point[0];
                        wallPoint[1] = wall.point[1];
                        wallPoint[2] = wall.point[2];
                    }
                }

                // W3 shared-edit (vehicles agent): the projectile step also sweeps
                // vehicle hulls; nearest of pawn/vehicle wins. Direct vehicle hits
                // take the base weapon's vsVehicleMult, then the warhead splashes
                // around the impact (vehicle excluded — no double dip).
                float vehPoint[3];
                float vehDist = 0.0f;
                const EntityId vehHit =
                    m_ctx->vehicles ? m_ctx->vehicles->RaycastVehicles(prev, dir, segLen, vehPoint, &vehDist) : 0;
                if (vehHit != 0 && (hit == 0 || vehDist < hitDist) && vehDist <= wallDist)
                {
                    const WeaponDef* base = m_ctx->data ? m_ctx->data->GetWeapon(p.weapon) : nullptr;
                    const float vdmg =
                        WeaponMath::LinearFalloff(p.damage, p.minDamage, p.falloffStartM, p.falloffEndM, p.traveledM) *
                        (base ? base->vsVehicleMult : 1.0f);
                    m_ctx->vehicles->ServerDamageVehicle(vehHit, vdmg, p.shooterPawn, p.shooter, p.weapon);
                    ServerBroadcastImpactFx(p.shooter, vehPoint, static_cast<uint8_t>(TFImpactSurface::Vehicle));
                    ExplodeAt(p, vehPoint, 0, vehHit);
                    it = m_projectiles.erase(it);
                    continue;
                }

                if (hit != 0 && hitDist <= wallDist)
                {
                    // Hit zone from the hit height on the victim's capsule (same
                    // rule as hitscan): head band, leg band, torso baseline.
                    PawnInfo victim;
                    bool head = false;
                    float zoneMult = 1.0f;
                    if (m_ctx->players && m_ctx->players->GetPawnByEntity(hit, victim) && victim.alive)
                    {
                        const Ballistics::HitZone zone = Ballistics::ClassifyHitZone(hitPoint, victim.pos);
                        head = zone == Ballistics::HitZone::Head;
                        zoneMult = Ballistics::ZoneDamageMult(zone, p.headshotMult);
                    }

                    const float damage =
                        WeaponMath::LinearFalloff(p.damage, p.minDamage, p.falloffStartM, p.falloffEndM, p.traveledM) *
                        zoneMult;

                    // W6 progression: projectile direct hit (splash never counts).
                    if (m_ctx->progression)
                        m_ctx->progression->ServerRecordHits(p.shooter, p.weapon, 1);
                    if (m_ctx->damage)
                        m_ctx->damage->ServerApplyDamage(hit, p.shooterPawn, p.shooter, damage, kDamageKindBullet,
                                                         p.weapon, head);
                    ServerBroadcastImpactFx(p.shooter, hitPoint, static_cast<uint8_t>(TFImpactSurface::Pawn));
                    ExplodeAt(p, hitPoint, hit);
                    dead = true;
                }
                else if (wallBlocked)
                {
                    // Warhead meets world geometry mid-flight: detonate on the wall.
                    // W11 impact-broadcast: a wall stop carrying a deployable
                    // entity is a shield hit; anything else is static world.
                    TFDeployableView dv;
                    const bool shieldStop = wall.entityId != 0 && m_ctx->deployables &&
                                            m_ctx->deployables->GetDeployable(static_cast<EntityId>(wall.entityId), dv);
                    ServerBroadcastImpactFx(
                        p.shooter, wallPoint,
                        static_cast<uint8_t>(shieldStop ? TFImpactSurface::Shield : TFImpactSurface::Static));
                    ExplodeAt(p, wallPoint, 0);
                    dead = true;
                }
            }

            // Terrain impact.
            if (!dead && m_ctx && m_ctx->world && p.pos[1] <= m_ctx->world->TerrainHeightAt(p.pos[0], p.pos[2]))
            {
                // W11 impact-broadcast: lift the fx point back onto the surface
                // (the integrated pos can be up to one step underground).
                const float surfPt[3] = {p.pos[0], m_ctx->world->TerrainHeightAt(p.pos[0], p.pos[2]), p.pos[2]};
                ServerBroadcastImpactFx(p.shooter, surfPt, static_cast<uint8_t>(TFImpactSurface::Terrain));
                ExplodeAt(p, p.pos, 0);
                dead = true;
            }

            if (!dead && p.lifeSec > kProjectileLifeSec)
                dead = true;

            it = dead ? m_projectiles.erase(it) : it + 1;
        }
    }

    void TFWeaponSystem::ExplodeAt(const ServerProjectile& p, const float at[3], EntityId excludeEntity,
                                   EntityId excludeVehicle)
    {
        if (p.splashRadiusM <= 0.0f || p.splashDamage <= 0.0f || !m_ctx || !m_ctx->players || !m_ctx->damage)
            return;

        // TF-W1-FULL: PawnInfo.pos comes from live ECS state; the registry stub
        // reports zeros, so splash is effectively inert until the full player
        // system lands. Logic is correct against real positions.
        m_ctx->players->ForEachAlivePawn(
            [&](const PawnInfo& pawn)
            {
                if (pawn.entity == excludeEntity)
                    return;
                const float d = std::sqrt(WeaponMath::Dist2(pawn.pos, at));
                if (d > p.splashRadiusM)
                    return;
                // Ballistics: splash line-of-sight — intervening world geometry (walls,
                // deployable shields) blocks the blast. Physics unavailable -> always
                // visible (pre-ballistics behaviour).
                const float chest[3] = {pawn.pos[0], pawn.pos[1] + WeaponMath::kPawnHeightM * 0.5f, pawn.pos[2]};
                if (!Ballistics::SplashVisible(m_ctx->engine, at, chest))
                    return;
                const float dmg = p.splashDamage * (1.0f - d / p.splashRadiusM);
                if (dmg > 1.0f)
                    m_ctx->damage->ServerApplyDamage(pawn.entity, p.shooterPawn, p.shooter, dmg, kDamageKindExplosive,
                                                     p.weapon, false);
            });

        // W3 shared-edit (deployables agent): explosive splash also damages
        // Fabricator/Medtech deployables with the same linear falloff. Direct
        // hitscan/projectile impact vs deployables stays TF-W4 (needs deployable
        // capsules in the lag-comp raycast set).
        if (m_ctx->deployables)
            m_ctx->deployables->ServerSplashDamageDeployables(at, p.splashRadiusM, p.splashDamage, p.shooter);

        // W3 shared-edit (vehicles agent): explosive splash also damages vehicle
        // hulls (rockets near-missing a tank still hurt it). vsVehicleMult comes
        // from the base weapon row; the direct-hit vehicle is excluded above.
        if (m_ctx->vehicles)
        {
            const WeaponDef* base = m_ctx->data ? m_ctx->data->GetWeapon(p.weapon) : nullptr;
            m_ctx->vehicles->ServerApplySplash(at, p.splashRadiusM, p.splashDamage, base ? base->vsVehicleMult : 1.0f,
                                               p.shooterPawn, p.shooter, p.weapon, excludeVehicle);
        }
    }

} // namespace Terrafront
