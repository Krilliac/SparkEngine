/**
 * @file TFProgressionSystem.cpp
 * @brief Server-authoritative XP/rank/flux progression + JSON persistence.
 *
 * XP flow: EvPlayerKilled (and TFRegionSystem calling ServerAwardXP directly)
 * -> ServerAwardXP -> rank recompute -> EvXPAwarded/EvRankUp on the bus +
 * TF_XPEvent to the owning client (TFDamageSystem send pattern).
 *
 * Flux flow: every continent fluxTickSec, each connected player with a
 * faction earns 1 base flux + max(1, heldFluxPerTick / factionPlayerCount)
 * when the faction holds any flux-producing region; wallet capped.
 *
 * Persistence: read-modify-write of the "progression" key inside
 * Saves/terrafront_state.json (the file is shared with territory state),
 * written atomically via tmp+rename.
 */
#include "Game/TFProgressionSystem.h"

#include "Account/TFCharacterSystem.h" // W5 onboarding (Task 6): re-key persistence
#include "Data/TFDataTables.h"
#include "Game/TFGrenadeSystem.h" // loadout-depth wave: grenade choice key constants (single source of truth)
#include "Game/TFPlayerSystem.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFServerSim.h"          // W5 onboarding (Task 6): ActiveCharacterOf
#include "Persistence/TFUnlockTree.h" // W6 progression expansion: unlock table
#include "World/TFRegionSystem.h"
#include "Utils/JsonUtils.h"
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
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr uint16_t kKillXP = 100; // DESIGN §4
        constexpr uint16_t kHeadshotBonusXP = 25;
        constexpr float kDefaultFluxTickSec = 60.0f;
        constexpr float kSaveDebounceSec = 2.0f;  // dirty -> disk within 2 s
        constexpr float kSavePeriodicSec = 30.0f; // safety-net flush cadence

        constexpr const char* kSaveDir = "Saves";
        constexpr const char* kSaveFile = "Saves/terrafront_state.json";
        constexpr const char* kTmpFile = "Saves/terrafront_state.prog.tmp";

        // loadout-depth wave: own lazy parse of the suit passive table (TFOpticsSystem
        // ::EnsureTable precedent — Data/TFDataTables.h is a frozen contended surface).
        constexpr const char* kSuitsJsonPath = "Assets/MMOFPS/Data/suits.json";

        /// Read a whole file into a string; false if it does not exist / can't open.
        bool ReadAllText(const char* path, std::string& out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in.is_open())
                return false;
            std::ostringstream ss;
            ss << in.rdbuf();
            out = ss.str();
            return true;
        }

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
    // Persistence (shared Saves/terrafront_state.json; "progression" key only)
    // ---------------------------------------------------------------------------

    bool TFProgressionSystem::LoadFromDisk()
    {
        std::string text;
        if (!ReadAllText(kSaveFile, text))
            return false; // first boot: nothing saved yet

        const Spark::Json::Value root = Spark::Json::Parse(text);
        if (!root.IsObject() || !root.HasKey("progression"))
            return false;

        const Spark::Json::Value& plist = root["progression"]["players"];
        if (!plist.IsArray())
            return false;

        for (size_t i = 0; i < plist.Size(); ++i)
        {
            const Spark::Json::Value& row = plist[i];
            if (!row.IsObject())
                continue;
            const auto id = static_cast<PlayerId>(row["id"].AsNumber(0.0));
            if (id == kInvalidPlayer)
                continue;
            Prog rec;
            rec.xp = static_cast<uint32_t>(row["xp"].AsNumber(0.0));
            rec.flux = std::min(kFluxWalletCap, static_cast<uint32_t>(row["flux"].AsNumber(0.0)));
            rec.rank = RankForXP(rec.xp); // rank derives from xp, not the file
            m_players[id] = rec;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] progression loaded: %zu players from %s", m_players.size(),
                       kSaveFile);
        return true;
    }

    bool TFProgressionSystem::SaveNow()
    {
        namespace fs = std::filesystem;

        // Read-modify-write so co-resident sections (territory) are preserved.
        Spark::Json::Value root;
        std::string text;
        if (ReadAllText(kSaveFile, text))
            root = Spark::Json::Parse(text);
        if (!root.IsObject())
            root = Spark::Json::Value::MakeObject();

        Spark::Json::Value plist = Spark::Json::Value::MakeArray();
        for (const auto& [id, rec] : m_players)
        {
            Spark::Json::Value row = Spark::Json::Value::MakeObject();
            row["id"] = Spark::Json::Value(static_cast<double>(id));
            row["xp"] = Spark::Json::Value(static_cast<double>(rec.xp));
            row["rank"] = Spark::Json::Value(static_cast<int>(rec.rank));
            row["flux"] = Spark::Json::Value(static_cast<double>(rec.flux));
            plist.PushBack(std::move(row));

            // W5 onboarding (Task 6): re-key progression to the entered
            // character. The in-session runtime state above stays PlayerId-keyed
            // (unchanged, low risk); TFCharacterSystem/TFDatabase become the
            // durable per-character store, keyed by the character bound at
            // enter-world (TFServerSim::HandleEnterWorld). Players who never
            // completed onboarding (ActiveCharacterOf==0, e.g. bots or a
            // pre-Task-6 session) simply are not persisted here.
            if (m_ctx->characters && m_ctx->serverSim)
            {
                const uint64_t charId = m_ctx->serverSim->ActiveCharacterOf(id);
                if (charId != 0)
                    m_ctx->characters->PersistProgress(charId, rec.xp, rec.rank, rec.flux);
            }
        }
        Spark::Json::Value prog = Spark::Json::Value::MakeObject();
        prog["note"] = Spark::Json::Value("PlayerIds are session-scoped in W2");
        prog["players"] = std::move(plist);
        root["progression"] = std::move(prog);

        // W6: flush dirty unlock/loadout/stat meta to the character db on the
        // same cadence. Swept over the whole store (not the m_players loop) so a
        // player whose only mutation was meta (e.g. a loadout save before first
        // spawn) is not skipped. Character-bound records only; charId==0 rows
        // (bots, standalone sessions) are session-scoped by design.
        if (m_ctx && m_ctx->db)
            m_meta.PersistAllDirty(*m_ctx->db);

        std::error_code ec;
        fs::create_directories(kSaveDir, ec);

        {
            std::ofstream out(kTmpFile, std::ios::binary | std::ios::trunc);
            if (!out.is_open())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed: cannot open %s", kTmpFile);
                return false;
            }
            out << Spark::Json::StringifyPretty(root);
            if (!out.good())
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed: short write to %s", kTmpFile);
                return false;
            }
        }

        fs::rename(kTmpFile, kSaveFile, ec); // atomic replace (MoveFileEx semantics)
        if (ec)
        {
            fs::remove(kSaveFile, ec);
            fs::rename(kTmpFile, kSaveFile, ec);
            if (ec)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] progression save failed: rename -> %s (%s)", kSaveFile,
                                ec.message().c_str());
                return false;
            }
        }

        m_dirty = false;
        m_sinceSave = 0.0f;
        ++m_saves;
        return true;
    }

    // ---------------------------------------------------------------------------
    // W6 progression expansion: unlock tree / per-weapon stats / loadout
    // ---------------------------------------------------------------------------

    bool TFProgressionSystem::IsUnlocked(PlayerId player, std::string_view unlockKey) const
    {
        return IsUnlockedInternal(player, unlockKey, 0);
    }

    bool TFProgressionSystem::IsUnlockedInternal(PlayerId player, std::string_view unlockKey, int depth) const
    {
        if (depth > 8) // prereq-chain runaway guard (table is author-checked; belt & braces)
            return false;

        const TFUnlockDef* def = TFUnlockTree::Find(unlockKey);
        if (!def)
            return false;

        if (const auto* meta = m_meta.Find(player))
            if (meta->unlocks.count(std::string(unlockKey)) != 0)
                return true; // explicitly purchased/granted

        if (def->fluxCost > 0)
            return false; // purchasable node without a purchase record

        if (RankOf(player) < def->requiredRank)
            return false;
        if (def->prereq && !IsUnlockedInternal(player, def->prereq, depth + 1))
            return false;
        return true; // free rank unlock, auto-granted
    }

    bool TFProgressionSystem::IsWeaponUnlocked(PlayerId player, WeaponId weapon) const
    {
        const std::string* key = WeaponKeyOf(weapon);
        if (!key)
        {
            // Tables not loaded yet: fail open (nothing else can resolve the id
            // either). Tables loaded but id unknown: fail closed.
            return !(m_ctx && m_ctx->data && m_ctx->data->IsLoaded());
        }
        const TFUnlockDef* node = TFUnlockTree::FindByWeaponKey(*key);
        if (!node)
            return true; // not in the tree == default kit
        return IsUnlocked(player, node->key);
    }

    bool TFProgressionSystem::IsVehicleUnlocked(PlayerId player, VehicleId vehicle) const
    {
        const TFUnlockDef* node = TFUnlockTree::FindByVehicle(vehicle);
        if (!node)
            return true; // not in the tree (Drifter) == always available
        return IsUnlocked(player, node->key);
    }

    TFUnlockResult TFProgressionSystem::ServerTryUnlock(PlayerId player, std::string_view unlockKey)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return TFUnlockResult::NotAuthority;

        const TFUnlockDef* def = TFUnlockTree::Find(unlockKey);
        if (!def)
            return TFUnlockResult::UnknownKey;
        if (IsUnlocked(player, unlockKey))
            return TFUnlockResult::AlreadyUnlocked;
        if (RankOf(player) < def->requiredRank)
            return TFUnlockResult::RankTooLow;
        if (def->prereq && !IsUnlocked(player, def->prereq))
            return TFUnlockResult::PrereqLocked;
        if (def->fluxCost > 0 && !ServerSpendFlux(player, def->fluxCost))
            return TFUnlockResult::InsufficientFlux;

        auto& meta = m_meta.Ensure(player);
        meta.unlocks.insert(def->key);
        meta.dirty = true;
        m_dirty = true; // prompt durability for purchases (2 s debounce path)

        SendXPEvent(player, 0, kXPReasonUnlock);
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u unlocked '%s' (%s, %u flux)", player, def->key,
                       def->name, def->fluxCost);
        return TFUnlockResult::Ok;
    }

    TFWeaponAggStats TFProgressionSystem::GetStats(PlayerId player, WeaponId weapon) const
    {
        const std::string* key = WeaponKeyOf(weapon);
        return key ? GetStatsByKey(player, *key) : TFWeaponAggStats{};
    }

    TFWeaponAggStats TFProgressionSystem::GetStatsByKey(PlayerId player, std::string_view weaponKey) const
    {
        if (const auto* meta = m_meta.Find(player))
        {
            auto it = meta->stats.find(std::string(weaponKey));
            if (it != meta->stats.end())
                return it->second;
        }
        return TFWeaponAggStats{};
    }

    const std::unordered_map<std::string, TFWeaponAggStats>* TFProgressionSystem::AllStats(PlayerId player) const
    {
        const auto* meta = m_meta.Find(player);
        return meta ? &meta->stats : nullptr;
    }

    void TFProgressionSystem::ServerRecordShots(PlayerId player, WeaponId weapon, uint16_t count)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer || count == 0)
            return;
        if (const std::string* key = WeaponKeyOf(weapon))
        {
            auto& meta = m_meta.Ensure(player);
            meta.stats[*key].shots += count;
            meta.dirty = true; // meta-only dirt: persists on the periodic sweep
        }
    }

    void TFProgressionSystem::ServerRecordHits(PlayerId player, WeaponId weapon, uint16_t count)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer || count == 0)
            return;
        if (const std::string* key = WeaponKeyOf(weapon))
        {
            auto& meta = m_meta.Ensure(player);
            meta.stats[*key].hits += count;
            meta.dirty = true;
        }
    }

    const TFLoadout* TFProgressionSystem::GetLoadout(PlayerId player) const
    {
        const auto* meta = m_meta.Find(player);
        return (meta && meta->loadout.Any()) ? &meta->loadout : nullptr;
    }

    bool TFProgressionSystem::ServerSetLoadout(PlayerId player, const TFLoadout& loadout)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return false;

        // Slot indices mirror TFWeaponSystem's Slot enum semantics: 0=primary,
        // 1=secondary, 2=tool. Empty keys mean "class default" and always pass.
        if (!ValidLoadoutSlotKey(player, loadout.primary, 0) || !ValidLoadoutSlotKey(player, loadout.secondary, 1) ||
            !ValidLoadoutSlotKey(player, loadout.tool, 2))
            return false;

        auto& meta = m_meta.Ensure(player);
        meta.loadout = loadout;
        meta.dirty = true;
        m_dirty = true; // prompt durability (2 s debounce path)
        return true;
    }

    const std::string* TFProgressionSystem::WeaponKeyOf(WeaponId weapon) const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return nullptr;
        const WeaponDef* def = m_ctx->data->GetWeapon(weapon);
        return def ? &def->key : nullptr;
    }

    bool TFProgressionSystem::ValidLoadoutSlotKey(PlayerId player, const std::string& weaponKey, int slot) const
    {
        if (weaponKey.empty())
            return true; // class default
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false; // can't validate a concrete key without tables: reject

        const WeaponDef* def = m_ctx->data->GetWeaponByKey(weaponKey);
        if (!def)
            return false;

        // Slot category must match what the slot holds.
        if (slot == 0)
        {
            if (def->slot != "rifle" && def->slot != "carbine" && def->slot != "lmg" && def->slot != "sniper" &&
                def->slot != "shotgun" && def->slot != "launcher")
                return false;
        }
        else if (slot == 1)
        {
            if (def->slot != "pistol")
                return false;
        }
        else
        {
            if (def->slot != "tool")
                return false;
        }

        // Faction pool: common ("ALL" == FactionId::None) or the player's own.
        if (def->faction != FactionId::None && m_ctx->players && def->faction != m_ctx->players->FactionOf(player))
            return false;

        return IsWeaponUnlocked(player, def->id);
    }

    // ---------------------------------------------------------------------------
    // loadout-depth wave: grenade choice + suit slot
    // ---------------------------------------------------------------------------

    void TFProgressionSystem::LoadSuitTable()
    {
        std::string text;
        if (!ReadAllText(kSuitsJsonPath, text))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] suits: cannot open %s (no passives)", kSuitsJsonPath);
            return;
        }
        const Spark::Json::Value root = Spark::Json::Parse(text);
        if (!root.IsObject() || !root.HasKey("suits") || !root["suits"].IsArray())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] suits: %s malformed (no passives)", kSuitsJsonPath);
            return;
        }
        const Spark::Json::Value& suits = root["suits"];
        for (size_t i = 0; i < suits.Size(); ++i)
        {
            const Spark::Json::Value& s = suits[i];
            if (!s.IsObject() || !s.HasKey("key"))
                continue;
            TFSuitDef def;
            def.key = s["key"].AsString({});
            if (def.key.empty())
                continue;
            def.name = s["name"].AsString(def.key);
            def.desc = s["desc"].AsString({});
            if (s.HasKey("shieldMult"))
                def.shieldMult = static_cast<float>(s["shieldMult"].AsNumber(1.0));
            if (s.HasKey("regenDelayMult"))
                def.regenDelayMult = static_cast<float>(s["regenDelayMult"].AsNumber(1.0));
            if (s.HasKey("reserveMult"))
                def.reserveMult = static_cast<float>(s["reserveMult"].AsNumber(1.0));
            if (s.HasKey("healthMult"))
                def.healthMult = static_cast<float>(s["healthMult"].AsNumber(1.0));
            m_suits.emplace(def.key, def);
        }
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] suits: %zu passives loaded from %s", m_suits.size(),
                       kSuitsJsonPath);
    }

    const TFSuitDef* TFProgressionSystem::SuitDefFor(PlayerId player) const
    {
        const TFLoadout* lo = GetLoadout(player);
        if (!lo || lo->suit.empty())
            return nullptr;
        const auto it = m_suits.find(lo->suit);
        return it != m_suits.end() ? &it->second : nullptr;
    }

    float TFProgressionSystem::SuitShieldMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->shieldMult : 1.0f;
    }

    float TFProgressionSystem::SuitRegenDelayMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->regenDelayMult : 1.0f;
    }

    float TFProgressionSystem::SuitReserveMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->reserveMult : 1.0f;
    }

    float TFProgressionSystem::SuitHealthMult(PlayerId player) const
    {
        const TFSuitDef* d = SuitDefFor(player);
        return d ? d->healthMult : 1.0f;
    }

    bool TFProgressionSystem::ValidGrenadeChoiceKey(PlayerId player, const std::string& weaponKey) const
    {
        if (weaponKey.empty())
            return true; // frag_grenade default
        if (weaponKey != kTFGrenadeKeySmoke && weaponKey != kTFGrenadeKeyFlash)
            return false; // only these two are player-selectable; frag is the empty-string default
        // Unlock-gated, mirroring ValidLoadoutSlotKey's weapon check (IsWeaponUnlocked
        // resolves through TFUnlockTree::FindByWeaponKey -> "gr_smoke"/"gr_flash").
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return false; // can't validate a concrete key without tables: reject
        const WeaponDef* def = m_ctx->data->GetWeaponByKey(weaponKey);
        return def && IsWeaponUnlocked(player, def->id);
    }

    bool TFProgressionSystem::ValidSuitChoiceKey(PlayerId player, const std::string& suitKey) const
    {
        if (suitKey.empty())
            return true; // no passive
        if (m_suits.find(suitKey) == m_suits.end())
            return false; // unknown suits.json key
        const TFUnlockDef* node = TFUnlockTree::FindBySuitKey(suitKey);
        if (!node)
            return true; // not in the tree (overshield) == default kit
        return IsUnlocked(player, node->key);
    }

    void TFProgressionSystem::ServerHandleLoadoutExtMsgRaw(PlayerId sender, const void* data, size_t size)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || size < sizeof(TF_LoadoutExtChange) || !data)
            return;
        TF_LoadoutExtChange msg{};
        std::memcpy(&msg, data, sizeof(msg));
        msg.grenadeKey[sizeof(msg.grenadeKey) - 1] = '\0';
        msg.suitKey[sizeof(msg.suitKey) - 1] = '\0';
        ServerSetLoadoutExt(sender, std::string(msg.grenadeKey), std::string(msg.suitKey));
    }

    bool TFProgressionSystem::ServerSetLoadoutExt(PlayerId player, const std::string& grenadeKey,
                                                  const std::string& suitKey)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || player == kInvalidPlayer)
            return false;
        if (!ValidGrenadeChoiceKey(player, grenadeKey) || !ValidSuitChoiceKey(player, suitKey))
            return false;

        // Merges into the existing TFLoadout in place — deliberately never
        // touches primary/secondary/tool (see the header note on why this is
        // a separate entry point from ServerSetLoadout).
        auto& meta = m_meta.Ensure(player);
        meta.loadout.grenade = grenadeKey;
        meta.loadout.suit = suitKey;
        meta.dirty = true;
        m_dirty = true; // prompt durability (2 s debounce path)
        return true;
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
