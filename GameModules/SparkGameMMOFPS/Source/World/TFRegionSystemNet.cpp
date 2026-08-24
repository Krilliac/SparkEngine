/**
 * @file TFRegionSystemNet.cpp
 * @brief TFRegionSystem wire + persistence: TF_RegionState/TF_CaptureTick
 *        broadcasts, late-join full bursts, the client mirror handlers, and
 *        the temp-file-replaced terrafront_territory.<continent-key>.json save. Core
 *        capture loop lives in TFRegionSystem.cpp (same class, split per the
 *        repo file-size rules — mirrors the TFReplication/-Client split).
 */
#include "World/TFRegionSystem.h"

#include "Data/TFDataTables.h"
#include "Persistence/TFSavePaths.h"
#include "Persistence/TFWorldSave.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace Terrafront
{

    namespace
    {
        constexpr int kSaveVersion = 1;
    } // namespace

    // ---------------------------------------------------------------------------
    // Persistence (authority only; shared Terrafront save root, tmp+rename atomic)
    // ---------------------------------------------------------------------------

    std::filesystem::path TFRegionSystem::SavePath() const
    {
        if (!m_ctx || !m_ctx->data || !m_ctx->data->IsLoaded())
            return {};
        return SavePaths::ContinentFile("terrafront_territory", m_ctx->data->GetContinent().key);
    }

    bool TFRegionSystem::LoadPersisted()
    {
        using Spark::Json::Value;
        using WorldSave::ReadStatus;
        const ContinentDef& continent = m_ctx->data->GetContinent();
        const std::filesystem::path path = SavePath();
        if (path.empty() || !SavePaths::IsValidContinentKey(continent.key))
        {
            m_persistBlocked = true;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] territory persistence refused: invalid continent key");
            return false;
        }

        Value root;
        std::string detail;
        ReadStatus status = WorldSave::ReadJson(path, continent.key, continent.name, false, root, detail);
        bool migratingLegacy = false;
        std::filesystem::path source = path;
        if (status == ReadStatus::Missing)
        {
            const std::filesystem::path candidates[] = {SavePaths::File("terrafront_territory.json"),
                                                        SavePaths::LegacyExecutableFile("terrafront_territory.json")};
            for (const std::filesystem::path& candidate : candidates)
            {
                if (candidate.empty() || candidate == path || candidate == source)
                    continue;
                Value legacyRoot;
                std::string legacyDetail;
                const ReadStatus legacyStatus =
                    WorldSave::ReadJson(candidate, continent.key, continent.name, true, legacyRoot, legacyDetail);
                if (legacyStatus == ReadStatus::Loaded)
                {
                    root = std::move(legacyRoot);
                    source = candidate;
                    status = ReadStatus::Loaded;
                    migratingLegacy = true;
                    break;
                }
                if (legacyStatus != ReadStatus::Missing && legacyStatus != ReadStatus::WrongContinent)
                    SPARK_LOG_WARN(Spark::LogCategory::Game,
                                   "[TF] legacy territory save %s was not migrated (%s); original preserved",
                                   SavePaths::Utf8ForLog(candidate).c_str(), legacyDetail.c_str());
            }
        }

        if (status == ReadStatus::Missing)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] no matching territory save at %s — seeding defaults",
                           SavePaths::Utf8ForLog(path).c_str());
            return false;
        }
        if (status == ReadStatus::WrongContinent)
        {
            m_persistBlocked = true;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] qualified territory save %s belongs to another continent; writes latched off",
                            SavePaths::Utf8ForLog(path).c_str());
            return false;
        }
        if (status != ReadStatus::Loaded)
        {
            m_persistBlocked = true;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] territory save %s is unreadable/corrupt (%s); writes latched off",
                            SavePaths::Utf8ForLog(path).c_str(), detail.c_str());
            return false;
        }

        uint32_t saveVersion = 0;
        if (root.HasKey("version") && !WorldSave::ReadUint32(root["version"], saveVersion))
        {
            m_persistBlocked = !migratingLegacy;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] territory save %s has an invalid schema version; %s",
                            SavePaths::Utf8ForLog(source).c_str(),
                            migratingLegacy ? "legacy preserved" : "writes latched off");
            return false;
        }
        if (saveVersion > static_cast<uint32_t>(kSaveVersion))
        {
            m_persistBlocked = !migratingLegacy;
            SPARK_LOG_ERROR(Spark::LogCategory::Game,
                            "[TF] territory save %s uses newer schema version %u (supported %d); %s",
                            SavePaths::Utf8ForLog(source).c_str(), saveVersion, kSaveVersion,
                            migratingLegacy ? "legacy preserved" : "writes latched off");
            return false;
        }
        // Version 0 is the sole older schema: the pre-versioned layout has the
        // same fields validated below and is rewritten as v1 only after every
        // field has passed validation. No other downgrade is inferred.
        const bool migratingSchema = saveVersion == 0;

        const size_t count = m_state.size();
        const Value& owners = root["owners"];
        uint32_t persistedCount = 0;
        if (!WorldSave::ReadUint32(root["regionCount"], persistedCount) || persistedCount != count ||
            !owners.IsArray() || owners.Size() != count)
        {
            m_persistBlocked = !migratingLegacy;
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] territory save %s has an invalid region lattice; %s",
                            SavePaths::Utf8ForLog(source).c_str(),
                            migratingLegacy ? "legacy preserved" : "writes latched off");
            return false;
        }

        const auto& regions = continent.regions;
        std::vector<FactionId> validatedOwners(count);
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t raw = 0;
            if (!WorldSave::ReadUint32(owners[i], raw) || raw >= static_cast<uint32_t>(FactionId::COUNT))
            {
                m_persistBlocked = !migratingLegacy;
                SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] territory owner %zu is malformed; %s", i,
                                migratingLegacy ? "legacy preserved" : "writes latched off");
                return false;
            }
            FactionId owner = static_cast<FactionId>(raw);
            if (i < regions.size() && regions[i].tier == "skyanchor")
            {
                if (owner != regions[i].homeFaction && !migratingLegacy && !migratingSchema)
                {
                    m_persistBlocked = true;
                    SPARK_LOG_ERROR(Spark::LogCategory::Game,
                                    "[TF] territory skyanchor owner %zu conflicts with its home faction", i);
                    return false;
                }
                owner = regions[i].homeFaction;
            }
            validatedOwners[i] = owner;
        }
        WorldSave::DominionState dominion;
        if (!root.HasKey("dominion"))
        {
            if (!migratingSchema)
            {
                m_persistBlocked = !migratingLegacy;
                return false;
            }
        }
        else if (!WorldSave::ReadDominionState(root["dominion"], !migratingSchema,
                                               static_cast<uint32_t>(FactionId::COUNT), dominion))
        {
            m_persistBlocked = !migratingLegacy;
            return false;
        }

        for (size_t i = 0; i < count; ++i)
        {
            m_state[i].owner = validatedOwners[i];
            m_state[i].capturing = FactionId::None;
            m_state[i].progress = 0.0f;
            m_state[i].contested = false;
        }

        m_domActive = dominion.active;
        m_domFaction = static_cast<FactionId>(dominion.faction);
        m_domEndsAt = m_time + dominion.remainingSec;

        if (migratingLegacy || migratingSchema)
        {
            m_dirty = true;
            if (!PersistNow())
            {
                m_persistBlocked = true;
                SPARK_LOG_ERROR(
                    Spark::LogCategory::Game,
                    "[TF] territory schema/location migration from %s could not be committed; writes latched off",
                    SavePaths::Utf8ForLog(source).c_str());
                return false;
            }
        }

        m_dirty = false;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] territory restored from %s (hash 0x%08X%s)",
                       SavePaths::Utf8ForLog(source).c_str(), TerritoryHash(), m_domActive ? ", dominion hold" : "");
        return true;
    }

    bool TFRegionSystem::PersistNow()
    {
        if (!m_ctx || !m_ctx->IsAuthority() || m_state.empty() || m_persistBlocked || !m_ctx->data ||
            !m_ctx->data->IsLoaded())
            return false;

        using Spark::Json::Value;
        Value root = Value::MakeObject();
        root["version"] = Value(kSaveVersion);
        root["continentKey"] = Value(m_ctx->data->GetContinent().key);
        root["continent"] =
            Value(m_ctx->data && m_ctx->data->IsLoaded() ? m_ctx->data->GetContinent().name : std::string());
        root["regionCount"] = Value(static_cast<int>(m_state.size()));
        Value owners = Value::MakeArray();
        for (const RegionState& st : m_state)
            owners.PushBack(Value(static_cast<int>(st.owner)));
        root["owners"] = owners;
        Value dom = Value::MakeObject();
        dom["active"] = Value(m_domActive);
        dom["faction"] = Value(static_cast<int>(m_domFaction));
        dom["remainingSec"] = Value(m_domActive ? std::max(0.0, m_domEndsAt - m_time) : 0.0);
        root["dominion"] = dom;

        const std::filesystem::path path = SavePath();
        std::string detail;
        if (!WorldSave::WriteJson(path, root, detail))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Game, "[TF] territory save failed for %s (%s)",
                            SavePaths::Utf8ForLog(path).c_str(), detail.c_str());
            return false;
        }

        m_dirty = false;
        return true;
    }

#ifdef ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Server side
    // ---------------------------------------------------------------------------

    bool TFRegionSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
    }

    bool TFRegionSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFRegionSystem::ServerPollNewClients()
    {
        // NetworkManager exposes no join callback registry; diff GetClients()
        // like TFServerSim/TFReplication do.
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        const auto& clients = nm.GetClients();

        for (const auto& [id, info] : clients)
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            SendFullStateTo(id);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] regions: client %u joined — %zu region states sent", id,
                           m_state.size());
        }

        for (auto it = m_knownClients.begin(); it != m_knownClients.end();)
        {
            if (!clients.contains(*it))
                it = m_knownClients.erase(it);
            else
                ++it;
        }
    }

    void TFRegionSystem::SendFullStateTo(PlayerId target)
    {
        for (size_t i = 0; i < m_state.size(); ++i)
            SendRegionState(target, i, /*reliable*/ true);
    }

    void TFRegionSystem::SendRegionState(PlayerId target, size_t idx, bool reliable)
    {
        if (!ServerNetActive() || idx >= m_state.size())
            return;
        const RegionState& st = m_state[idx];

        TF_RegionState msg{};
        msg.regionId = static_cast<uint16_t>(idx);
        msg.owner = static_cast<uint8_t>(st.owner);
        msg.contested = st.contested ? 1 : 0;
        msg.captureProgress = st.progress;
        msg.capturingFaction = static_cast<uint8_t>(st.capturing);
        for (uint32_t k = 0; k < kMaxCapturePoints; ++k)
            msg.pointOwners[k] = static_cast<uint8_t>(st.owner); // W2: region-level model
        SendNet(target, static_cast<uint16_t>(TFMsg::RegionState), &msg, sizeof(msg), reliable);
    }

    void TFRegionSystem::SendCaptureTick(size_t idx)
    {
        if (!ServerNetActive() || idx >= m_state.size())
            return;
        const RegionState& st = m_state[idx];

        TF_CaptureTick msg{};
        msg.regionId = static_cast<uint16_t>(idx);
        msg.capturingFaction = static_cast<uint8_t>(st.capturing);
        msg.contested = st.contested ? 1 : 0;
        msg.progress = st.progress;
        SendNet(kInvalidPlayer, static_cast<uint16_t>(TFMsg::CaptureTick), &msg, sizeof(msg), /*reliable*/ false);
        ++m_ticksSent;
    }

    void TFRegionSystem::SendNet(PlayerId target, uint16_t msgId, const void* payload, size_t size, bool reliable)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized())
            return;
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
    // Client mirror
    // ---------------------------------------------------------------------------

    void TFRegionSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();

        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(TFMsg::RegionState)),
                           [this](const NetworkMessage& m) { OnNetRegionState(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(TFMsg::CaptureTick)),
                           [this](const NetworkMessage& m) { OnNetCaptureTick(m.payload.data(), m.payload.size()); });

        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] region mirror handlers registered");
    }

    void TFRegionSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace our handlers with no-ops
        // so no dangling `this` survives module shutdown (TFServerSim pattern).
        using Spark::Net::MessageType;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (TFMsg id : {TFMsg::RegionState, TFMsg::CaptureTick})
        {
            nm.RegisterHandler(static_cast<MessageType>(static_cast<uint16_t>(id)),
                               [](const Spark::Net::NetworkMessage&) {});
        }
        m_clientHandlers = false;
    }

    void TFRegionSystem::OnNetRegionState(const void* data, size_t size)
    {
        if (size != sizeof(TF_RegionState))
        {
            ++m_badPackets;
            return;
        }
        TF_RegionState msg;
        std::memcpy(&msg, data, sizeof(msg));
        if (msg.regionId >= m_state.size())
        {
            ++m_badPackets;
            return;
        }

        RegionState& st = m_state[msg.regionId];
        const FactionId oldOwner = st.owner;
        const bool oldContested = st.contested;

        const auto owner = static_cast<FactionId>(msg.owner);
        const auto capturing = static_cast<FactionId>(msg.capturingFaction);
        st.owner = owner < FactionId::COUNT ? owner : FactionId::None;
        st.capturing = capturing < FactionId::COUNT ? capturing : FactionId::None;
        st.progress = std::clamp(msg.captureProgress, 0.0f, 1.0f);
        st.contested = msg.contested != 0;
        st.lastNetAt = m_time; // tf_capture_debug: replicated-progress age
        ++m_stateRx;

        // Mirror-side notifications so UI systems (map/HUD) can react without
        // touching the wire themselves.
        if (m_events && st.owner != oldOwner)
            m_events->Fire(EvRegionCaptured{msg.regionId, st.owner, oldOwner});
        if (m_events && st.contested != oldContested)
            m_events->Fire(EvRegionContested{msg.regionId, st.contested});
    }

    void TFRegionSystem::OnNetCaptureTick(const void* data, size_t size)
    {
        if (size != sizeof(TF_CaptureTick))
        {
            ++m_badPackets;
            return;
        }
        TF_CaptureTick msg;
        std::memcpy(&msg, data, sizeof(msg));
        if (msg.regionId >= m_state.size())
        {
            ++m_badPackets;
            return;
        }

        RegionState& st = m_state[msg.regionId];
        const bool oldContested = st.contested;
        const auto capturing = static_cast<FactionId>(msg.capturingFaction);
        st.capturing = capturing < FactionId::COUNT ? capturing : FactionId::None;
        st.progress = std::clamp(msg.progress, 0.0f, 1.0f);
        st.contested = msg.contested != 0;
        st.lastNetAt = m_time; // tf_capture_debug: replicated-progress age
        ++m_tickRx;

        if (m_events && st.contested != oldContested)
            m_events->Fire(EvRegionContested{msg.regionId, st.contested});
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
