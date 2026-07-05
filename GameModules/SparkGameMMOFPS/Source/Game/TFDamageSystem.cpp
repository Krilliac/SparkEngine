/**
 * @file TFDamageSystem.cpp
 * @brief Server-authoritative damage: shield-first absorb, faction regen
 *        delay, friendly-fire policy, kill credit + client feedback messages.
 */
#include "Game/TFDamageSystem.h"
#include "Game/TFPlayerSystem.h"
#include "Data/TFDataTables.h"
#include "Net/TFNetProtocol.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include <imgui.h>
#endif

#include <cstring>

namespace Terrafront {

namespace {
constexpr float kFriendlyFireMult = 0.5f;   // DESIGN §4
constexpr float kShieldRegenPerSec = 80.0f;
}

TFDamageSystem::TFDamageSystem() = default;
TFDamageSystem::~TFDamageSystem() { if (m_initialized) Shutdown(); }

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
}

// ----------------------------------------------------------------------------

void TFDamageSystem::ServerApplyDamage(EntityId victim, EntityId attackerPawn,
                                       PlayerId attackerPlayer, float amount, uint8_t kind,
                                       WeaponId weapon, bool headshot)
{
    if (!m_ctx || !m_ctx->IsAuthority() || amount <= 0.0f)
        return;

    auto it = m_pools.find(victim);
    if (it == m_pools.end() || it->second.health <= 0.0f)
        return;
    HealthRec& rec = it->second;

    // Friendly fire: same faction (and not self) does reduced damage + TK tally.
    const FactionId attackerFaction =
        (m_ctx->players && attackerPlayer != kInvalidPlayer)
            ? m_ctx->players->FactionOf(attackerPlayer) : FactionId::None;
    const bool friendly = attackerFaction != FactionId::None &&
                          attackerFaction == rec.faction && attackerPawn != victim;
    if (friendly)
        amount *= kFriendlyFireMult;

    // Shield absorbs first.
    const float toShield = std::min(rec.shield, amount);
    rec.shield -= toShield;
    float remaining = amount - toShield;
    rec.health = std::max(0.0f, rec.health - remaining);
    rec.lastDamageAt = m_clock;

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
        SPARK_LOG_WARN(Spark::LogCategory::Game,
                       "[TF] Team kill by player %u (total %u)", attackerPlayer, tks);
        // TF-W2: grief kick at 10 TKs / 15 min.
    }

    if (m_ctx->players)
        m_ctx->players->ServerKillPawn(victim, attackerPlayer, weapon, headshot);

    BroadcastKill(attackerPlayer, rec.owner, weapon, attackerFaction, rec.faction, headshot);
    m_pools.erase(it);
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
    (void)owner; (void)msgId; (void)payload; (void)size;
#endif
}

void TFDamageSystem::BroadcastKill(PlayerId killer, PlayerId victim, WeaponId weapon,
                                   FactionId killerF, FactionId victimF, bool headshot)
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
            msg.type = static_cast<Spark::Net::MessageType>(
                static_cast<uint16_t>(TFMsg::KillEvent));
            msg.channel = Spark::Net::ChannelType::Reliable;
            msg.payload.resize(sizeof(ke));
            std::memcpy(msg.payload.data(), &ke, sizeof(ke));
            nm.SendToAll(msg);
        }
    }
#else
    (void)killer; (void)victim; (void)weapon; (void)killerF; (void)victimF; (void)headshot;
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
#endif
}

} // namespace Terrafront
