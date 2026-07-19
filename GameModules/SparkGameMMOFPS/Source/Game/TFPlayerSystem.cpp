/**
 * @file TFPlayerSystem.cpp
 * @brief Pawn registry + server spawn/kill/respawn flow, W1 full ECS
 *        implementation.
 *
 * Authority: pawns are real ECS entities (Transform + HealthComponent +
 * NetworkIdentity + TF components from Game/TFComponents.h). TFServerSim
 * integrates movement and writes the pawn Transform each fixed tick
 * (position = feet, rotation.y = yaw in degrees) — PawnInfo reads that
 * Transform as the position/yaw truth; velocity/pitch come from
 * TFPawnMoveComp when a writer fills it (zeros until then).
 *
 * Query surface + the pure-client half (RemotePawn discovery, visuals,
 * debug panel) live in TFPlayerSystemClient.cpp; the spawn-request flow
 * (TF_SpawnRequest validation, spawn-point selection, TF_SpawnReply send)
 * lives in TFPlayerSystemSpawn.cpp (same class, split per repo file-size
 * rules).
 */
#include "Game/TFPlayerSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFProgressionSystem.h" // loadout-depth wave: suit passive scalars applied at spawn
#include "Game/TFServerValidation.h"  // W13 anti-cheat lane: recycled-PlayerId counter hygiene
#include "Net/TFServerSim.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace Terrafront
{

    namespace
    {

        constexpr float kRadToDeg = 57.2957795f;

        // Synthetic network-entity ids used only when no ECS world exists (headless
        // unit tests without an engine world). Real builds always take the ECS path.
        EntityId g_nextSyntheticEntity = 1000000;

    } // namespace

    TFPlayerSystem::TFPlayerSystem() = default;
    TFPlayerSystem::~TFPlayerSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFPlayerSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFPlayerSystem initialized");
        return true;
    }

    void TFPlayerSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;

        if (m_ctx->IsAuthority())
            UpdateAuthorityLifecycle();
        else
            SyncClientRecords();
    }

    void TFPlayerSystem::FixedUpdate(float) {}

    void TFPlayerSystem::Shutdown()
    {
        for (auto& [player, rec] : m_players)
            DestroyPawn(rec);
        m_players.clear();
        m_pawnOwner.clear();
        m_pendingLocalPawn = 0;
        m_initialized = false;
    }

    double TFPlayerSystem::NowSec() const
    {
        return (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
    }

    // ---------------------------------------------------------------- server ops

    uint32_t TFPlayerSystem::CreatePawnEntity(PlayerId player, FactionId faction, ClassId cls, const float pos[3],
                                              float yaw)
    {
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world)
            return 0;

        EntityID e = world->CreateEntity("TF_Pawn_P" + std::to_string(player));
        if (static_cast<uint32_t>(e) == 0)
        {
            // Entity id 0 means "no pawn" in the frozen W1 contract — park it on a
            // never-destroyed placeholder and take the next id for the pawn.
            e = world->CreateEntity("TF_Pawn_P" + std::to_string(player));
        }

        Transform& t = world->AddComponent<Transform>(e);
        t.position = {pos[0], pos[1], pos[2]};
        t.rotation.y = yaw * kRadToDeg;

        const ClassDef* cd = (m_ctx->data && m_ctx->data->IsLoaded()) ? m_ctx->data->GetClass(cls) : nullptr;
        float maxHealth = cd ? cd->health : 500.0f;
        float maxShield = cd ? cd->shield : 500.0f;

        // loadout-depth wave: suit passive scalars, applied ONCE here — the
        // exact seam ClassDef health/shield/ammo stats already reach the pawn
        // (TFProgressionSystem::Suit*Mult; 1.0 == no suit / no progression
        // pointer, e.g. headless unit tests that construct pawns directly).
        if (m_ctx->progression)
        {
            maxHealth *= m_ctx->progression->SuitHealthMult(player);
            maxShield *= m_ctx->progression->SuitShieldMult(player);
        }

        HealthComponent& hc = world->AddComponent<HealthComponent>(e);
        hc.health = maxHealth;
        hc.maxHealth = maxHealth;

        NetworkIdentity& ni = world->AddComponent<NetworkIdentity>(e);
        ni.networkID = static_cast<uint32_t>(e);
        ni.ownerClientID = player;
        ni.isLocalAuthority = true; // this process is the simulation authority

        world->AddComponent<TFFactionComp>(e).faction = faction;
        world->AddComponent<TFClassComp>(e).cls = cls;
        world->AddComponent<TFPawnComp>(e).owner = player;

        TFShieldComp& sh = world->AddComponent<TFShieldComp>(e);
        sh.cur = maxShield;
        sh.max = maxShield;
        if (const FactionDef* fd =
                (m_ctx->data && m_ctx->data->IsLoaded()) ? m_ctx->data->GetFaction(faction) : nullptr)
        {
            sh.regenDelay = fd->shieldRegenDelaySec;
            if (m_ctx->progression)
                sh.regenDelay = std::max(0.0f, sh.regenDelay * m_ctx->progression->SuitRegenDelayMult(player));
        }

        TFPawnMoveComp& mv = world->AddComponent<TFPawnMoveComp>(e);
        mv.yaw = yaw;

        TFWeaponHeldComp& wh = world->AddComponent<TFWeaponHeldComp>(e);
        wh.weaponId = PickDefaultWeapon(faction, cls);
        if (const WeaponDef* wd =
                (wh.weaponId != kInvalidWeapon && m_ctx->data) ? m_ctx->data->GetWeapon(wh.weaponId) : nullptr)
        {
            wh.ammoMag = wd->magSize;
            wh.ammoPool = wd->reserve;
            if (m_ctx->progression) // loadout-depth wave: suit reserve-ammo passive
                wh.ammoPool = static_cast<int>(std::lround(wh.ammoPool * m_ctx->progression->SuitReserveMult(player)));
        }

        // First-person local pawns render no body (W1; TF-W4: shadow-only body).
        const bool isLocalFirstPerson = m_ctx->HasLocalPlayer() && player == m_ctx->localPlayer;
        if (!isLocalFirstPerson)
            AttachPawnVisual(static_cast<uint32_t>(e), faction, cls);

        return static_cast<uint32_t>(e);
    }

    EntityId TFPlayerSystem::ServerSpawnPawn(PlayerId player, FactionId faction, ClassId cls, const float pos[3],
                                             float yaw)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return 0;

        PlayerRec& rec = m_players[player];
        if (rec.hasPawn)
            DestroyPawn(rec);

        const uint32_t local = CreatePawnEntity(player, faction, cls, pos, yaw);
        if (local == 0)
        {
            // FAIL LOUD: with no ECS entity every PawnInfo read for this pawn
            // collapses to pos (0,0,0) / hp 0 — movement replication still "works"
            // (TFServerSim MoveState), so the breakage is silent but fatal for
            // anything position-driven (region capture occupancy, AoE damage,
            // capture XP radius). Root cause seen 2026-07-10: the headless engine
            // boot registered no World service, so every dedicated-server pawn sat
            // at the origin and capture never ticked.
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] pawn for player %u has NO ECS entity (engine GetWorld() null?) — position-driven "
                           "gameplay (capture occupancy, AoE, XP radius) is BROKEN for this pawn",
                           player);
            Spark::SimpleConsole::GetInstance().LogWarning(
                "[TF] WARNING: pawn spawned without an ECS entity — capture/AoE will not work (no engine World)");
        }

        rec.faction = faction;
        rec.cls = cls;
        rec.local = local;
        rec.pawn = (local != 0) ? local : g_nextSyntheticEntity++;
        rec.hasPawn = true;
        rec.alive = true;
        rec.nextRespawnAt = 0.0;
        rec.despawnAt = 0.0;
        m_pawnOwner[rec.pawn] = player;

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] pawn %u spawned for player %u (%s class %u) at (%.0f %.0f %.0f)",
                       rec.pawn, player, FactionTag(faction), static_cast<unsigned>(cls), pos[0], pos[1], pos[2]);
        {
            // Console echo with immediate ECS read-back: catches silent
            // write/read divergence (a real bug class here — see 2026-07-05
            // smoke where every PawnInfo read collapsed to (0,0,0)).
            PawnInfo probe{};
            char msg[160];
            if (FillPawnInfo(player, rec, probe))
                std::snprintf(msg, sizeof(msg),
                              "[TF] spawn: p%u %s at (%.0f %.0f %.0f) readback (%.1f %.1f %.1f) hp %.0f", player,
                              FactionTag(faction), pos[0], pos[1], pos[2], probe.pos[0], probe.pos[1], probe.pos[2],
                              probe.health);
            else
                std::snprintf(msg, sizeof(msg), "[TF] spawn: p%u readback FAILED", player);
            Spark::SimpleConsole::GetInstance().LogInfo(msg);
        }

        if (m_events)
            m_events->Fire(EvPlayerSpawned{player, rec.pawn, cls, faction});
        return rec.pawn;
    }

    void TFPlayerSystem::ServerKillPawn(EntityId victim, PlayerId killerPlayer, WeaponId weapon, bool headshot)
    {
        auto it = m_pawnOwner.find(victim);
        if (it == m_pawnOwner.end())
            return;
        const PlayerId victimPlayer = it->second;
        PlayerRec& rec = m_players[victimPlayer];
        if (!rec.alive)
            return;

        rec.alive = false;
        rec.nextRespawnAt = NowSec() + kTFRespawnDelaySec;
        rec.despawnAt = NowSec() + kTFDespawnDelaySec;

        ServerSetPawnHealth(victim, 0.0f, 0.0f);

        // Console kill feed — the war's audit trail for exec_results.log.
        Spark::SimpleConsole::GetInstance().LogInfo("[TF] kill: p" + std::to_string(victimPlayer) + " (" +
                                                    FactionTag(rec.faction) + ") by p" + std::to_string(killerPlayer) +
                                                    (headshot ? " HS" : ""));

        if (m_events)
        {
            m_events->Fire(EvPlayerKilled{victimPlayer, killerPlayer, weapon, headshot});
            if (m_ctx->HasLocalPlayer() && victimPlayer == m_ctx->localPlayer)
                m_events->Fire(EvLocalPlayerDied{victimPlayer, kTFRespawnDelaySec});
        }
    }

    void TFPlayerSystem::ServerSetPawnHealth(EntityId entity, float health, float shield)
    {
        uint32_t local = 0;
        if (!ResolveEntity(entity, local))
            return;
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        const auto e = static_cast<EntityID>(local);
        if (!world || !world->GetRegistry().valid(e))
            return;

        if (HealthComponent* hc = world->GetComponent<HealthComponent>(e))
        {
            hc->health = std::max(0.0f, health);
            hc->isDead = hc->health <= 0.0f;
        }
        if (TFShieldComp* sc = world->GetComponent<TFShieldComp>(e))
        {
            sc->cur = std::clamp(shield, 0.0f, sc->max);
            sc->sinceDamage = 0.0f;
        }
    }

    void TFPlayerSystem::ServerHandleFactionSelect(PlayerId player, FactionId faction)
    {
        m_players[player].faction = faction;
    }

    void TFPlayerSystem::ServerHandlePlayerDisconnect(PlayerId player)
    {
        // W13 anti-cheat lane: drop this player's violation counters here too
        // (independent of TFServerSim::CleanupPlayerSession's own call — this
        // is a second, non-networked entry point into the same disconnect
        // flow) so a recycled PlayerId never inherits a stranger's history.
        TFServerValidation::Get().ClearPlayer(player);

        auto it = m_players.find(player);
        if (it == m_players.end())
            return;
        DestroyPawn(it->second);
        m_players.erase(it);
    }

    void TFPlayerSystem::UpdateAuthorityLifecycle()
    {
        // Corpse despawn: dead pawns keep their entity (visible corpse; the
        // replication destroy already went out on kill) until the hide window ends.
        const double now = NowSec();
        for (auto& [player, rec] : m_players)
        {
            if (rec.hasPawn && !rec.alive && rec.despawnAt > 0.0 && now >= rec.despawnAt)
                DestroyPawn(rec);
        }
    }

    // ---------------------------------------------------------------- helpers

    WeaponId TFPlayerSystem::PickDefaultWeapon(FactionId faction, ClassId cls) const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return kInvalidWeapon;
        const ClassDef* cd = m_ctx->data->GetClass(cls);
        if (!cd || cd->primarySlots.empty())
            return kInvalidWeapon;
        for (const auto& w : m_ctx->data->AllWeapons())
            if (w.slot == cd->primarySlots.front() && (w.faction == faction || w.faction == FactionId::None))
                return w.id;
        return kInvalidWeapon;
    }

    void TFPlayerSystem::DestroyPawn(PlayerRec& rec)
    {
        if (!rec.hasPawn)
            return;

        if (rec.local != 0)
        {
            World* world = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetWorld() : nullptr;
            const auto e = static_cast<EntityID>(rec.local);
            if (world && world->GetRegistry().valid(e))
                world->DestroyEntity(e);
        }

        m_pawnOwner.erase(rec.pawn);
        rec.hasPawn = false;
        rec.alive = false;
        rec.pawn = 0;
        rec.local = 0;
        rec.despawnAt = 0.0;
    }

} // namespace Terrafront
