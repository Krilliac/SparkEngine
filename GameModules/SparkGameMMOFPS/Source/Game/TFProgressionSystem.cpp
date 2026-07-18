/**
 * @file TFProgressionSystem.cpp
 * @brief Server-authoritative XP/rank/flux progression core: lifecycle, the
 *        W2 contract API, XP sources, flux income, the W5 durable
 *        per-character re-attach and wire feedback.
 *
 * XP flow: EvPlayerKilled (and TFRegionSystem calling ServerAwardXP directly)
 * -> ServerAwardXP -> rank recompute -> EvXPAwarded/EvRankUp on the bus +
 * TF_XPEvent to the owning client (TFDamageSystem send pattern).
 *
 * Flux flow: every continent fluxTickSec, each connected player with a
 * faction earns 1 base flux + max(1, heldFluxPerTick / factionPlayerCount)
 * when the faction holds any flux-producing region; wallet capped.
 *
 * Split parts (repo file-size rules): JSON persistence (read-modify-write of
 * the "progression" key inside Saves/terrafront_state.json, tmp+rename) in
 * TFProgressionSystemPersist.cpp; W6 unlock tree / per-weapon stats /
 * loadout + the loadout-depth wave (grenade choice, suit slot) in
 * TFProgressionSystemMeta.cpp; shared helpers in
 * TFProgressionSystemInternal.h.
 */
#include "Game/TFProgressionSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFServerSim.h"        // W5 onboarding (Task 6): ActiveCharacterOf
#include "Persistence/TFDatabase.h" // W5 onboarding (Task 6): TFCharacterRecord re-attach
#include "World/TFRegionSystem.h"
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

        constexpr uint16_t kKillXP = 100; // DESIGN §4
        constexpr uint16_t kHeadshotBonusXP = 25;
        constexpr float kDefaultFluxTickSec = 60.0f;
        constexpr float kSaveDebounceSec = 2.0f;  // dirty -> disk within 2 s
        constexpr float kSavePeriodicSec = 30.0f; // safety-net flush cadence

    } // namespace

    TFProgressionSystem::TFProgressionSystem() = default;
    TFProgressionSystem::~TFProgressionSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFProgressionSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        // Rank curve: cumulative XP to HOLD rank N. Rank 1 is the floor (0 XP);
        // rank N>=2 needs 500 * N^1.6 (precomputed once, DESIGN §4 "XP curve").
        m_rankXP[0] = 0;
        m_rankXP[1] = 0;
        for (uint16_t n = 2; n <= kTFMaxRank; ++n)
            m_rankXP[n] = static_cast<uint32_t>(std::llround(500.0 * std::pow(double(n), 1.6)));

        events.Subscribe<EvPlayerKilled>([this](const EvPlayerKilled& ev) { OnPlayerKilled(ev); });
        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });

        LoadFromDisk();
        LoadSuitTable(); // loadout-depth wave

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] TFProgressionSystem initialized (%zu persisted players, rank30=%u XP)", m_players.size(),
                       m_rankXP[kTFMaxRank]);
        return true;
    }

    void TFProgressionSystem::Update(float deltaTime)
    {
        // NOTE: Main.cpp does not route FixedUpdate to this system; all periodic
        // work (flux income, save flush) is paced here on the frame update.
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        float tickSec = kDefaultFluxTickSec;
        if (m_ctx->data && m_ctx->data->IsLoaded() && m_ctx->data->GetContinent().fluxTickSec > 0.0f)
            tickSec = m_ctx->data->GetContinent().fluxTickSec;

        m_fluxAccum += deltaTime;
        if (m_fluxAccum >= tickSec)
        {
            m_fluxAccum -= tickSec;
            FluxIncomeTick();
        }

        m_sinceSave += deltaTime;
        if (m_dirty && (m_sinceSave >= kSaveDebounceSec))
            SaveNow();
        else if (m_sinceSave >= kSavePeriodicSec) // periodic safety net
        {
            // W6: meta-only dirt (per-shot stat bumps) deliberately rides this
            // slower cadence instead of the 2 s debounce — otherwise any sustained
            // fire would rewrite the save + db files every 2 s. Unlock purchases
            // and loadout changes set m_dirty and take the fast path.
            if (m_dirty || m_meta.AnyDirty())
                SaveNow();
            m_sinceSave = 0.0f;
        }
    }

    void TFProgressionSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
    }

    void TFProgressionSystem::Shutdown()
    {
        if (m_ctx && m_ctx->IsAuthority() && (m_dirty || m_meta.AnyDirty()))
            SaveNow();
        m_players.clear();
        m_meta.Clear();
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Contract API
    // ---------------------------------------------------------------------------

    TFProgressionSystem::Prog& TFProgressionSystem::Ensure(PlayerId player)
    {
        return m_players[player]; // default rank 1 / 0 xp / 0 flux
    }

    uint16_t TFProgressionSystem::RankForXP(uint32_t xp) const
    {
        for (uint16_t n = kTFMaxRank; n >= 2; --n)
            if (xp >= m_rankXP[n])
                return n;
        return 1;
    }

    uint32_t TFProgressionSystem::XPForRank(uint16_t rank) const
    {
        return m_rankXP[std::clamp<uint16_t>(rank, 1, kTFMaxRank)];
    }

    void TFProgressionSystem::ServerAwardXP(PlayerId player, uint16_t amount, uint8_t reasonCode)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return;

        Prog& rec = Ensure(player);
        rec.xp += amount;
        ++m_awards;
        m_dirty = true;

        const uint16_t newRank = RankForXP(rec.xp);
        const bool rankedUp = newRank > rec.rank;
        rec.rank = newRank;

        if (m_events)
        {
            m_events->Fire(EvXPAwarded{player, amount, reasonCode});
            if (rankedUp)
                m_events->Fire(EvRankUp{player, newRank});
        }
        if (rankedUp)
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u ranked up to %u (%u XP)", player, newRank, rec.xp);

        SendXPEvent(player, amount, reasonCode);
    }

    uint32_t TFProgressionSystem::FluxOf(PlayerId player) const
    {
        auto it = m_players.find(player);
        return it != m_players.end() ? it->second.flux : 0;
    }

    bool TFProgressionSystem::ServerSpendFlux(PlayerId player, uint32_t amount)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return false;
        auto it = m_players.find(player);
        if (it == m_players.end() || it->second.flux < amount)
            return false;
        it->second.flux -= amount;
        m_dirty = true;
        SendXPEvent(player, 0, kXPReasonSync); // wallet refresh to the client
        return true;
    }

    void TFProgressionSystem::ServerGrantFlux(PlayerId player, uint32_t amount)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return;
        auto& rec = m_players[player];
        rec.flux = std::min<uint32_t>(kFluxWalletCap, rec.flux + amount);
        m_dirty = true;
        SendXPEvent(player, 0, kXPReasonSync);
    }

    uint16_t TFProgressionSystem::RankOf(PlayerId player) const
    {
        auto it = m_players.find(player);
        return it != m_players.end() ? it->second.rank : 1;
    }

    uint32_t TFProgressionSystem::XPOf(PlayerId player) const
    {
        auto it = m_players.find(player);
        return it != m_players.end() ? it->second.xp : 0;
    }

    // ---------------------------------------------------------------------------
    // W5 onboarding: durable per-character re-attach (final-review #1/#2)
    // ---------------------------------------------------------------------------

    void TFProgressionSystem::ServerLoadCharacter(PlayerId player, uint32_t xp, uint16_t rank, uint32_t flux)
    {
        if (!m_initialized || player == kInvalidPlayer)
            return;

        // Overwrites (not merges) any pre-existing runtime record for this
        // PlayerId -- e.g. a stale roster entry from a recycled id, or the
        // rank-1/0-xp default OnPlayerSpawned would otherwise create. Must run
        // BEFORE spawn/save so no default record can round-trip to disk first.
        Prog& rec = m_players[player];
        rec.xp = xp;
        rec.rank = std::clamp<uint16_t>(rank, 1, kTFMaxRank);
        rec.flux = std::min<uint32_t>(kFluxWalletCap, flux);

        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] progression seeded from character record for player %u: "
                       "xp=%u rank=%u flux=%u",
                       player, rec.xp, rec.rank, rec.flux);

        // W6: seed unlocks / loadout / per-weapon stats from the same durable
        // character. TFServerSim::HandleEnterWorld binds m_activeCharacter BEFORE
        // calling us (TFServerSim.cpp), so ActiveCharacterOf already resolves.
        if (m_ctx->db && m_ctx->serverSim)
        {
            const uint64_t charId = m_ctx->serverSim->ActiveCharacterOf(player);
            TFCharacterRecord record;
            if (charId != 0 && m_ctx->db->FindCharacter(charId, record))
                m_meta.SeedFromRecord(player, record);
        }
    }

    void TFProgressionSystem::ClearPlayer(PlayerId player)
    {
        // W6: flush this player's meta to their character BEFORE dropping the
        // runtime record — TFServerSim::CleanupPlayerSession has already flushed
        // xp/rank/flux via PersistProgress but does not know about meta, and by
        // the time it calls us the m_activeCharacter binding is gone (we rely on
        // the charId cached in the meta record at seed time instead).
        if (m_ctx && m_ctx->db)
            m_meta.PersistIfDirty(player, *m_ctx->db);
        m_meta.Erase(player);
        m_players.erase(player);
    }

    // ---------------------------------------------------------------------------
    // XP sources
    // ---------------------------------------------------------------------------

    void TFProgressionSystem::OnPlayerKilled(const EvPlayerKilled& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        if (ev.killer == kInvalidPlayer || ev.killer == ev.victim)
            return; // environment death / suicide: no XP

        // No XP for team kills (friendly fire is on at 50%, DESIGN §4).
        if (m_ctx->players)
        {
            const FactionId kf = m_ctx->players->FactionOf(ev.killer);
            const FactionId vf = m_ctx->players->FactionOf(ev.victim);
            if (kf != FactionId::None && kf == vf)
                return;
        }

        // W6: per-weapon aggregates (kills + headshot kills) ride the same
        // legitimacy guards as kill XP — no team kills, no suicides.
        if (const std::string* key = WeaponKeyOf(ev.weapon))
        {
            auto& meta = m_meta.Ensure(ev.killer);
            TFWeaponAggStats& s = meta.stats[*key];
            ++s.kills;
            if (ev.headshot)
                ++s.headshots;
            meta.dirty = true;
        }

        ServerAwardXP(ev.killer, static_cast<uint16_t>(kKillXP + (ev.headshot ? kHeadshotBonusXP : 0)), kXPReasonKill);
    }

    void TFProgressionSystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority())
            return;
        Ensure(ev.player);                        // roster entry for flux income
        SendXPEvent(ev.player, 0, kXPReasonSync); // totals refresh (reconnect/persist)
    }

    // ---------------------------------------------------------------------------
    // Flux income
    // ---------------------------------------------------------------------------

    void TFProgressionSystem::FluxIncomeTick()
    {
        if (!m_ctx || !m_ctx->players)
            return;

        // Held flux production + connected player counts, per faction.
        uint32_t fluxSum[static_cast<size_t>(FactionId::COUNT)] = {};
        uint32_t heads[static_cast<size_t>(FactionId::COUNT)] = {};

        if (m_ctx->regions && m_ctx->data && m_ctx->data->IsLoaded())
        {
            for (const RegionDef& rd : m_ctx->data->GetContinent().regions)
            {
                const FactionId owner = m_ctx->regions->OwnerOf(rd.id);
                if (owner != FactionId::None && rd.fluxPerTick > 0)
                    fluxSum[static_cast<size_t>(owner)] += static_cast<uint32_t>(rd.fluxPerTick);
            }
        }

        for (const auto& [id, rec] : m_players)
        {
            const FactionId f = m_ctx->players->FactionOf(id);
            if (f != FactionId::None)
                ++heads[static_cast<size_t>(f)];
        }

        // Disconnected players read back FactionId::None (TFPlayerSystem erases
        // their record), so they neither count heads nor accrue income.
        for (auto& [id, rec] : m_players)
        {
            const FactionId f = m_ctx->players->FactionOf(id);
            if (f == FactionId::None)
                continue;

            const uint32_t sum = fluxSum[static_cast<size_t>(f)];
            const uint32_t count = std::max(1u, heads[static_cast<size_t>(f)]);
            const uint32_t bonus = sum > 0 ? std::max(1u, sum / count) : 0;
            const uint32_t next = std::min(kFluxWalletCap, rec.flux + 1u + bonus);
            if (next == rec.flux)
                continue;
            rec.flux = next;
            m_dirty = true;
            SendXPEvent(id, 0, kXPReasonFluxTick);
        }
    }

    // ---------------------------------------------------------------------------
    // Wire feedback (TFDamageSystem::SendToOwner pattern)
    // ---------------------------------------------------------------------------

    void TFProgressionSystem::SendXPEvent(PlayerId player, uint16_t amount, uint8_t reason)
    {
        auto it = m_players.find(player);
        if (it == m_players.end())
            return;
        TF_XPEvent xp{};
        xp.amount = amount;
        xp.reasonCode = reason;
        xp.newTotalXP = it->second.xp;
        xp.newRank = it->second.rank;
        xp.fluxWallet = static_cast<uint16_t>(std::min<uint32_t>(it->second.flux, 0xFFFFu));
        SendToOwner(player, static_cast<uint16_t>(TFMsg::XPEvent), &xp, sizeof(xp));
    }

    void TFProgressionSystem::SendToOwner(PlayerId owner, uint16_t msgId, const void* payload, size_t size)
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

    // ---------------------------------------------------------------------------
    // Debug UI
    // ---------------------------------------------------------------------------

    void TFProgressionSystem::RenderDebugUI()
    {
#ifdef SPARK_HAS_IMGUI
        if (!ImGui::CollapsingHeader("TF Progression"))
            return;
        ImGui::Text("players : %zu  awards: %u  saves: %u%s", m_players.size(), m_awards, m_saves,
                    m_dirty ? " (dirty)" : "");
        ImGui::Text("flux tick accum: %.0fs", m_fluxAccum);
        for (const auto& [id, rec] : m_players)
            ImGui::Text("  P%u: rank %2u  xp %6u  flux %3u", id, rec.rank, rec.xp, rec.flux);
        ImGui::Text("meta records: %zu%s", m_meta.Count(), m_meta.AnyDirty() ? " (dirty)" : "");
        for (const auto& [id, meta] : m_meta.AllMeta())
        {
            uint32_t kills = 0, shots = 0, hits = 0;
            for (const auto& entry : meta.stats)
            {
                kills += entry.second.kills;
                shots += entry.second.shots;
                hits += entry.second.hits;
            }
            ImGui::Text("  P%u: char %llu  unlocks %zu  K/S/H %u/%u/%u  loadout [%s|%s|%s]", id,
                        static_cast<unsigned long long>(meta.charId), meta.unlocks.size(), kills, shots, hits,
                        meta.loadout.primary.empty() ? "-" : meta.loadout.primary.c_str(),
                        meta.loadout.secondary.empty() ? "-" : meta.loadout.secondary.c_str(),
                        meta.loadout.tool.empty() ? "-" : meta.loadout.tool.c_str());
        }
#endif
    }

} // namespace Terrafront
