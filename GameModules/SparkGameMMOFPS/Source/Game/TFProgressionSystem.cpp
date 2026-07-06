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

#include "Account/TFCharacterSystem.h"   // W5 onboarding (Task 6): re-key persistence
#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFServerSim.h"             // W5 onboarding (Task 6): ActiveCharacterOf
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

namespace Terrafront {

namespace {

constexpr uint16_t kKillXP          = 100;   // DESIGN §4
constexpr uint16_t kHeadshotBonusXP = 25;
constexpr float    kDefaultFluxTickSec = 60.0f;
constexpr float    kSaveDebounceSec    = 2.0f;   // dirty -> disk within 2 s
constexpr float    kSavePeriodicSec    = 30.0f;  // safety-net flush cadence

constexpr const char* kSaveDir  = "Saves";
constexpr const char* kSaveFile = "Saves/terrafront_state.json";
constexpr const char* kTmpFile  = "Saves/terrafront_state.prog.tmp";

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
TFProgressionSystem::~TFProgressionSystem() { if (m_initialized) Shutdown(); }

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

    m_initialized = true;
    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] TFProgressionSystem initialized (%zu persisted players, rank30=%u XP)",
                   m_players.size(), m_rankXP[kTFMaxRank]);
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
    else if (m_sinceSave >= kSavePeriodicSec)   // periodic safety net
    {
        if (m_dirty)
            SaveNow();
        m_sinceSave = 0.0f;
    }
}

void TFProgressionSystem::FixedUpdate(float fixedDeltaTime) { (void)fixedDeltaTime; }

void TFProgressionSystem::Shutdown()
{
    if (m_ctx && m_ctx->IsAuthority() && m_dirty)
        SaveNow();
    m_players.clear();
    m_initialized = false;
}

// ---------------------------------------------------------------------------
// Contract API
// ---------------------------------------------------------------------------

TFProgressionSystem::Prog& TFProgressionSystem::Ensure(PlayerId player)
{
    return m_players[player];   // default rank 1 / 0 xp / 0 flux
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
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u ranked up to %u (%u XP)",
                       player, newRank, rec.xp);

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
    SendXPEvent(player, 0, kXPReasonSync);   // wallet refresh to the client
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

void TFProgressionSystem::ServerLoadCharacter(PlayerId player, uint32_t xp, uint16_t rank,
                                              uint32_t flux)
{
    if (!m_initialized || player == kInvalidPlayer)
        return;

    // Overwrites (not merges) any pre-existing runtime record for this
    // PlayerId -- e.g. a stale roster entry from a recycled id, or the
    // rank-1/0-xp default OnPlayerSpawned would otherwise create. Must run
    // BEFORE spawn/save so no default record can round-trip to disk first.
    Prog& rec = m_players[player];
    rec.xp   = xp;
    rec.rank = std::clamp<uint16_t>(rank, 1, kTFMaxRank);
    rec.flux = std::min<uint32_t>(kFluxWalletCap, flux);

    SPARK_LOG_INFO(Spark::LogCategory::Game,
                   "[TF] progression seeded from character record for player %u: "
                   "xp=%u rank=%u flux=%u", player, rec.xp, rec.rank, rec.flux);
}

void TFProgressionSystem::ClearPlayer(PlayerId player)
{
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
        return;   // environment death / suicide: no XP

    // No XP for team kills (friendly fire is on at 50%, DESIGN §4).
    if (m_ctx->players)
    {
        const FactionId kf = m_ctx->players->FactionOf(ev.killer);
        const FactionId vf = m_ctx->players->FactionOf(ev.victim);
        if (kf != FactionId::None && kf == vf)
            return;
    }

    ServerAwardXP(ev.killer,
                  static_cast<uint16_t>(kKillXP + (ev.headshot ? kHeadshotBonusXP : 0)),
                  kXPReasonKill);
}

void TFProgressionSystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
{
    if (!m_ctx || !m_ctx->IsAuthority())
        return;
    Ensure(ev.player);                       // roster entry for flux income
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
    uint32_t heads[static_cast<size_t>(FactionId::COUNT)]   = {};

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

        const uint32_t sum   = fluxSum[static_cast<size_t>(f)];
        const uint32_t count = std::max(1u, heads[static_cast<size_t>(f)]);
        const uint32_t bonus = sum > 0 ? std::max(1u, sum / count) : 0;
        const uint32_t next  = std::min(kFluxWalletCap, rec.flux + 1u + bonus);
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
    xp.amount     = amount;
    xp.reasonCode = reason;
    xp.newTotalXP = it->second.xp;
    xp.newRank    = it->second.rank;
    xp.fluxWallet = static_cast<uint16_t>(std::min<uint32_t>(it->second.flux, 0xFFFFu));
    SendToOwner(player, static_cast<uint16_t>(TFMsg::XPEvent), &xp, sizeof(xp));
}

void TFProgressionSystem::SendToOwner(PlayerId owner, uint16_t msgId,
                                      const void* payload, size_t size)
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

// ---------------------------------------------------------------------------
// Persistence (shared Saves/terrafront_state.json; "progression" key only)
// ---------------------------------------------------------------------------

bool TFProgressionSystem::LoadFromDisk()
{
    std::string text;
    if (!ReadAllText(kSaveFile, text))
        return false;   // first boot: nothing saved yet

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
        rec.xp   = static_cast<uint32_t>(row["xp"].AsNumber(0.0));
        rec.flux = std::min(kFluxWalletCap,
                            static_cast<uint32_t>(row["flux"].AsNumber(0.0)));
        rec.rank = RankForXP(rec.xp);   // rank derives from xp, not the file
        m_players[id] = rec;
    }

    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] progression loaded: %zu players from %s",
                   m_players.size(), kSaveFile);
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
        row["id"]   = Spark::Json::Value(static_cast<double>(id));
        row["xp"]   = Spark::Json::Value(static_cast<double>(rec.xp));
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
    prog["note"]    = Spark::Json::Value("PlayerIds are session-scoped in W2");
    prog["players"] = std::move(plist);
    root["progression"] = std::move(prog);

    std::error_code ec;
    fs::create_directories(kSaveDir, ec);

    {
        std::ofstream out(kTmpFile, std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] progression save failed: cannot open %s", kTmpFile);
            return false;
        }
        out << Spark::Json::StringifyPretty(root);
        if (!out.good())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] progression save failed: short write to %s", kTmpFile);
            return false;
        }
    }

    fs::rename(kTmpFile, kSaveFile, ec);   // atomic replace (MoveFileEx semantics)
    if (ec)
    {
        fs::remove(kSaveFile, ec);
        fs::rename(kTmpFile, kSaveFile, ec);
        if (ec)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] progression save failed: rename -> %s (%s)",
                            kSaveFile, ec.message().c_str());
            return false;
        }
    }

    m_dirty = false;
    m_sinceSave = 0.0f;
    ++m_saves;
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
    ImGui::Text("players : %zu  awards: %u  saves: %u%s",
                m_players.size(), m_awards, m_saves, m_dirty ? " (dirty)" : "");
    ImGui::Text("flux tick accum: %.0fs", m_fluxAccum);
    for (const auto& [id, rec] : m_players)
        ImGui::Text("  P%u: rank %2u  xp %6u  flux %3u", id, rec.rank, rec.xp, rec.flux);
#endif
}

} // namespace Terrafront
