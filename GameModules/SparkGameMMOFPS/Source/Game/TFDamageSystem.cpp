/**
 * @file TFDamageSystem.cpp
 * @brief Server-authoritative damage: shield-first absorb, faction regen
 *        delay, friendly-fire policy, kill credit + client feedback messages.
 */
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Data/TFDataTables.h"
#include "Net/TFNetProtocol.h"
#include "World/TFSanctuaryZone.h" // continents lane: sanctuary no-damage rule
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

namespace Terrafront
{

    namespace
    {
        constexpr float kFriendlyFireMult = 0.5f; // DESIGN §4
        constexpr float kShieldRegenPerSec = 80.0f;
    } // namespace

    TFDamageSystem::TFDamageSystem() = default;
    TFDamageSystem::~TFDamageSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFDamageSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;
        m_initialized = true;

        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPawnSpawned(ev); });
        return true;
    }

    void TFDamageSystem::Update(float) {}

    void TFDamageSystem::FixedUpdate(float fixedDeltaTime)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;

        m_clock += fixedDeltaTime;

        // Shield regen after the faction's regen delay without damage.
        for (auto& [pawn, rec] : m_pools)
        {
            if (rec.noRegen || rec.shield >= rec.maxShield || rec.health <= 0.0f)
                continue;
            if (m_clock - rec.lastDamageAt < rec.regenDelaySec)
                continue;
            rec.shield = std::min(rec.maxShield, rec.shield + kShieldRegenPerSec * fixedDeltaTime);
            if (m_ctx->players)
                m_ctx->players->ServerSetPawnHealth(pawn, rec.health, rec.shield);
        }
    }

    void TFDamageSystem::Shutdown()
    {
        m_pools.clear();
        m_teamKills.clear();
        m_damageLog.clear(); // death-recap lane (W11)
        m_recapMirror = nullptr;
        m_killcamMirror = nullptr; // killcam lane (W13)
        m_initialized = false;
    }

    void TFDamageSystem::OnPawnSpawned(const EvPlayerSpawned& ev)
    {
        HealthRec rec;
        rec.owner = ev.player;
        rec.faction = ev.faction;

        if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded())
        {
            if (const ClassDef* cd = m_ctx->data->GetClass(ev.cls))
            {
                rec.health = rec.maxHealth = cd->health;
                rec.shield = rec.maxShield = cd->shield;
                rec.noRegen = cd->noRegen;
            }
            if (const FactionDef* fd = m_ctx->data->GetFaction(ev.faction))
                rec.regenDelaySec = fd->shieldRegenDelaySec;
        }
        m_pools[ev.pawn] = rec;
    }

    bool TFDamageSystem::GetPools(EntityId pawn, float& outHealth, float& outShield) const
    {
        auto it = m_pools.find(pawn);
        if (it == m_pools.end())
            return false;
        outHealth = it->second.health;
        outShield = it->second.shield;
        return true;
    }

    // --- W3 shared-edit additions (deployables/colossus agent) ------------------

    void TFDamageSystem::ServerHeal(EntityId pawn, float amount)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
            return;
        auto it = m_pools.find(pawn);
        if (it == m_pools.end() || it->second.health <= 0.0f)
            return; // unknown or dead — healing never revives
        HealthRec& rec = it->second;
        if (rec.health >= rec.maxHealth)
            return;
        rec.health = std::min(rec.maxHealth, rec.health + amount);
        if (m_ctx->players)
            m_ctx->players->ServerSetPawnHealth(pawn, rec.health, rec.shield);
    }

    void TFDamageSystem::ServerForgetPawn(EntityId pawn)
    {
        m_pools.erase(pawn);
        m_damageLog.erase(pawn); // death-recap lane (W11): no stale log across suit swaps
    }

    // ----------------------------------------------------------------------------

    void TFDamageSystem::ServerApplyDamage(EntityId victim, EntityId attackerPawn, PlayerId attackerPlayer,
                                           float amount, uint8_t kind, WeaponId weapon, bool headshot)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
            return;

        auto it = m_pools.find(victim);
        if (it == m_pools.end() || it->second.health <= 0.0f)
            return;
        HealthRec& rec = it->second;

        // Sanctuary Haven is a no-damage zone (continents lane, additive rule):
        // pawns standing inside the reserved sanctuary rectangle can neither take
        // nor deal damage — all three factions share the staging ground. Positional
        // check (TFSanctuaryZone.h) so it holds for every damage kind, including
        // splash/pain-field with no attacker pawn.
        if (m_ctx->players)
        {
            PawnInfo p{};
            if (m_ctx->players->GetPawnByEntity(victim, p) && TFTravel_IsInSanctuary(p.pos[0], p.pos[2]))
                return;
            if (attackerPawn != 0 && m_ctx->players->GetPawnByEntity(attackerPawn, p) &&
                TFTravel_IsInSanctuary(p.pos[0], p.pos[2]))
                return;
        }

        // Friendly fire: same faction (and not self) does reduced damage + TK tally.
        const FactionId attackerFaction = (m_ctx->players && attackerPlayer != kInvalidPlayer)
                                              ? m_ctx->players->FactionOf(attackerPlayer)
                                              : FactionId::None;
        const bool friendly =
            attackerFaction != FactionId::None && attackerFaction == rec.faction && attackerPawn != victim;
        if (friendly)
            amount *= kFriendlyFireMult;

        // class-abilities lane (W9): ability damage seam (Bulwark Field absorb).
        // The installed filter can only REDUCE the hit (clamped); a fully
        // absorbed hit still counts as damage for the shield-regen delay —
        // the pawn WAS hit — but deals nothing and gives no kill credit.
        if (m_incomingFilter)
        {
            const float filtered = m_incomingFilter(victim, amount);
            amount = std::max(0.0f, std::min(filtered, amount));
            if (amount <= 0.0f)
            {
                rec.lastDamageAt = m_clock;
                return;
            }
        }

        // Shield absorbs first.
        const float toShield = std::min(rec.shield, amount);
        rec.shield -= toShield;
        float remaining = amount - toShield;
        rec.health = std::max(0.0f, rec.health - remaining);
        rec.lastDamageAt = m_clock;

        // death-recap lane (W11): per-pawn rolling damage log. Recorded BEFORE
        // the kill branch so the fatal hit is part of the victim's timeline.
        {
            TFDamageLogHit hit;
            hit.attackerPlayer = attackerPlayer;
            hit.weapon = weapon;
            hit.amount = amount;
            hit.kind = kind;
            hit.headshot = headshot;
            hit.at = m_clock;
            m_damageLog[victim].Push(hit);
        }

        if (m_ctx->players)
            m_ctx->players->ServerSetPawnHealth(victim, rec.health, rec.shield);

        const bool killed = rec.health <= 0.0f;

        // Attacker feedback (hitmarker).
        if (attackerPlayer != kInvalidPlayer)
        {
            TF_HitConfirm hc{};
            hc.victimEntity = victim;
            hc.damage = static_cast<uint16_t>(std::min(amount, 65535.0f));
            hc.headshot = headshot ? 1 : 0;
            hc.killed = killed ? 1 : 0;
            SendToOwner(attackerPlayer, static_cast<uint16_t>(TFMsg::HitConfirm), &hc, sizeof(hc));
        }

        // Victim feedback (damage direction handled client-side W2; octant 0 W1).
        TF_DamageEvent de{};
        de.attackerEntity = attackerPawn;
        de.damage = static_cast<uint16_t>(std::min(amount, 65535.0f));
        de.damageKind = kind;
        de.dirOctant = 0;
        SendToOwner(rec.owner, static_cast<uint16_t>(TFMsg::DamageEvent), &de, sizeof(de));

        if (m_events)
            m_events->Fire(EvPlayerDamaged{victim, attackerPawn, amount, kind});

        if (!killed)
            return;

        ++m_killCount;
        if (friendly && attackerPlayer != kInvalidPlayer)
        {
            const uint32_t tks = ++m_teamKills[attackerPlayer];
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] Team kill by player %u (total %u)", attackerPlayer, tks);
            // TF-W2: grief kick at 10 TKs / 15 min.
        }

        // death-recap lane (W11): assemble + deliver the victim's recap while
        // the pawn registry still holds both pawns (ServerKillPawn despawns).
        SendDeathRecap(victim, rec, attackerPawn, attackerPlayer, attackerFaction);

        // killcam lane (W13): server-truth killer pose snapshot to the victim,
        // captured next to the recap so the killer pawn registry entry is still
        // the freshest truth (before ServerKillPawn despawns anything).
        SendKillcam(rec, attackerPawn, attackerPlayer);

        if (m_ctx->players)
            m_ctx->players->ServerKillPawn(victim, attackerPlayer, weapon, headshot);

        BroadcastKill(attackerPlayer, rec.owner, weapon, attackerFaction, rec.faction, headshot);
        m_pools.erase(it);
        m_damageLog.erase(victim); // the dead pawn's log is spent
    }

    void TFDamageSystem::SendDeathRecap(EntityId victim, const HealthRec& rec, EntityId attackerPawn,
                                        PlayerId attackerPlayer, FactionId attackerFaction)
    {
        if (rec.owner == kInvalidPlayer)
            return; // ownerless pawn — nobody to show a recap to

        TF_DeathRecap rc{};
        rc.killerPlayer = attackerPlayer;
        rc.killerClass = static_cast<uint8_t>(ClassId::COUNT);
        rc.killerFaction = static_cast<uint8_t>(attackerFaction);
        rc.killerHealth = 0xFFFFu;
        rc.killerShield = 0xFFFFu;
        rc.distanceDm = 0xFFFFu;

        // Timeline: the victim's window-filtered log, oldest first.
        auto logIt = m_damageLog.find(victim);
        if (logIt != m_damageLog.end())
        {
            TFDamageLogHit rows[kTFDeathRecapMaxEntries];
            const uint32_t n = logIt->second.Collect(m_clock, kTFDeathRecapWindowSec, rows);
            for (uint32_t i = 0; i < n; ++i)
            {
                TF_DeathRecapEntry& e = rc.entries[i];
                e.attackerPlayer = rows[i].attackerPlayer;
                e.weaponId = rows[i].weapon;
                e.damage = static_cast<uint16_t>(std::min(rows[i].amount, 65535.0f));
                const double age = std::max(0.0, m_clock - rows[i].at);
                e.ageDeciSec = static_cast<uint16_t>(std::min(age * 10.0, 65535.0));
                e.headshot = rows[i].headshot ? 1 : 0;
                e.damageKind = rows[i].kind;
            }
            rc.entryCount = static_cast<uint8_t>(n);
        }

        // Killer summary: class + distance from the pawn registry, remaining
        // pools from our own records, "damage dealt to killer" from the
        // KILLER's rolling log (the victim's hits on them, same window).
        if (attackerPawn != 0)
        {
            PawnInfo killerPi{};
            if (m_ctx->players && m_ctx->players->GetPawnByEntity(attackerPawn, killerPi))
            {
                rc.killerClass = static_cast<uint8_t>(killerPi.cls);
                PawnInfo victimPi{};
                if (m_ctx->players->GetPawnByEntity(victim, victimPi))
                {
                    const float dx = killerPi.pos[0] - victimPi.pos[0];
                    const float dy = killerPi.pos[1] - victimPi.pos[1];
                    const float dz = killerPi.pos[2] - victimPi.pos[2];
                    const float distM = std::sqrt(dx * dx + dy * dy + dz * dz);
                    rc.distanceDm = static_cast<uint16_t>(std::min(distM * 10.0f, 65534.0f));
                }
            }
            auto killerPools = m_pools.find(attackerPawn);
            if (killerPools != m_pools.end())
            {
                rc.killerHealth = static_cast<uint16_t>(std::clamp(killerPools->second.health, 0.0f, 65534.0f));
                rc.killerShield = static_cast<uint16_t>(std::clamp(killerPools->second.shield, 0.0f, 65534.0f));
            }
            auto killerLog = m_damageLog.find(attackerPawn);
            if (killerLog != m_damageLog.end())
            {
                float dmg = 0.0f;
                uint32_t hits = 0;
                killerLog->second.SumFrom(rec.owner, m_clock, kTFDeathRecapWindowSec, dmg, hits);
                rc.damageToKiller = static_cast<uint16_t>(std::min(dmg, 65535.0f));
                rc.hitsOnKiller = static_cast<uint8_t>(std::min(hits, 255u));
            }
        }

        ++m_recapsSent;

        // Listen-host/standalone local victim: no socket — direct mirror into
        // the UI panel (TFMedalSystem::SendMedalToOwner pattern).
        if (m_ctx->HasLocalPlayer() && rec.owner == m_ctx->localPlayer)
        {
            if (m_recapMirror)
                m_recapMirror(rc);
            return;
        }
        SendToOwner(rec.owner, kTFMsgDeathRecap, &rc, sizeof(rc));
    }

    void TFDamageSystem::SendKillcam(const HealthRec& rec, EntityId attackerPawn, PlayerId attackerPlayer)
    {
        if (rec.owner == kInvalidPlayer)
            return; // ownerless pawn — nobody to show a killcam to
        if (attackerPlayer == kInvalidPlayer || attackerPlayer == rec.owner)
            return; // environment death or suicide — no killcam target

        TF_KillcamData kc{};
        kc.killerPlayer = attackerPlayer;
        kc.killerEntity = static_cast<uint32_t>(attackerPawn);
        kc.hasPose = 0;

        // Server-truth killer pose, straight from the pawn registry while it is
        // still live (SendDeathRecap runs immediately before ServerKillPawn).
        if (attackerPawn != 0 && m_ctx->players)
        {
            PawnInfo killerPi{};
            if (m_ctx->players->GetPawnByEntity(attackerPawn, killerPi))
            {
                kc.killerPos[0] = killerPi.pos[0];
                kc.killerPos[1] = killerPi.pos[1];
                kc.killerPos[2] = killerPi.pos[2];
                kc.killerYawRad = killerPi.yaw; // PawnInfo::yaw is radians (TFSpectator precedent)
                kc.hasPose = 1;
            }
        }

        ++m_killcamsSent;

        // Listen-host/standalone local victim: no socket — direct mirror into
        // the client killcam (SendDeathRecap mirror pattern).
        if (m_ctx->HasLocalPlayer() && rec.owner == m_ctx->localPlayer)
        {
            if (m_killcamMirror)
                m_killcamMirror(kc);
            return;
        }
        SendToOwner(rec.owner, kTFMsgKillcamData, &kc, sizeof(kc));
    }

    void TFDamageSystem::SendToOwner(PlayerId owner, uint16_t msgId, const void* payload, size_t size)
    {
#ifdef ENABLE_NETWORKING
        if (owner == kInvalidPlayer || !m_ctx || m_ctx->role == NetRole::Standalone)
            return;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(size);
        std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(owner, msg);
#else
        (void)owner;
        (void)msgId;
        (void)payload;
        (void)size;
#endif
    }

    void TFDamageSystem::BroadcastKill(PlayerId killer, PlayerId victim, WeaponId weapon, FactionId killerF,
                                       FactionId victimF, bool headshot)
    {
#ifdef ENABLE_NETWORKING
        if (m_ctx && m_ctx->role != NetRole::Standalone)
        {
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.IsInitialized())
            {
                TF_KillEvent ke{};
                ke.killerPlayer = killer;
                ke.victimPlayer = victim;
                ke.weaponId = weapon;
                ke.killerFaction = static_cast<uint8_t>(killerF);
                ke.victimFaction = static_cast<uint8_t>(victimF);
                ke.headshot = headshot ? 1 : 0;

                Spark::Net::NetworkMessage msg;
                msg.type = static_cast<Spark::Net::MessageType>(static_cast<uint16_t>(TFMsg::KillEvent));
                msg.channel = Spark::Net::ChannelType::Reliable;
                msg.payload.resize(sizeof(ke));
                std::memcpy(msg.payload.data(), &ke, sizeof(ke));
                nm.SendToAll(msg);
            }
        }
#else
        (void)killer;
        (void)victim;
        (void)weapon;
        (void)killerF;
        (void)victimF;
        (void)headshot;
#endif
    }

    void TFDamageSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Damage"))
            return;
        ImGui::Text("tracked pawns : %zu", m_pools.size());
        ImGui::Text("kills         : %u", m_killCount);
        ImGui::Text("TK offenders  : %zu", m_teamKills.size());
        ImGui::Text("damage logs   : %zu", m_damageLog.size()); // death-recap lane (W11)
        ImGui::Text("recaps sent   : %u", m_recapsSent);
        ImGui::Text("killcams sent : %u", m_killcamsSent); // killcam lane (W13)
#endif
    }

} // namespace Terrafront
