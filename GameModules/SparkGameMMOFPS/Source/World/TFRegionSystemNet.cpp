/**
 * @file TFRegionSystemNet.cpp
 * @brief TFRegionSystem wire + persistence: TF_RegionState/TF_CaptureTick
 *        broadcasts, late-join full bursts, the client mirror handlers, and
 *        the atomic Saves/terrafront_territory.json territory save. Core
 *        capture loop lives in TFRegionSystem.cpp (same class, split per the
 *        repo file-size rules — mirrors the TFReplication/-Client split).
 */
#include "World/TFRegionSystem.h"

#include "Data/TFDataTables.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {
        constexpr int kSaveVersion = 1;
    } // namespace

    // ---------------------------------------------------------------------------
    // Persistence (authority only; JSON next to the exe, tmp+rename atomic)
    // ---------------------------------------------------------------------------

    std::string TFRegionSystem::SavePath() const
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path exeDir;
#ifdef _WIN32
        char buf[MAX_PATH]{};
        const DWORD len = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
            exeDir = fs::path(std::string(buf, len)).parent_path();
#endif
        if (exeDir.empty())
            exeDir = fs::current_path(ec); // non-Windows / lookup failure fallback
        return (exeDir / "Saves" / "terrafront_territory.json").string();
    }

    bool TFRegionSystem::LoadPersisted()
    {
        const std::string path = SavePath();
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open())
        {
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] no territory save at %s — seeding from regions.json",
                           path.c_str());
            return false;
        }
        std::ostringstream ss;
        ss << f.rdbuf();

        using Spark::Json::Value;
        const Value root = Spark::Json::Parse(ss.str());
        if (!root.IsObject())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] territory save %s is malformed — ignored", path.c_str());
            return false;
        }

        const size_t count = m_state.size();
        const Value& owners = root["owners"];
        if (static_cast<size_t>(root["regionCount"].AsInt(-1)) != count || !owners.IsArray() || owners.Size() != count)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] territory save %s does not match the region table "
                           "(%zu regions) — reseeded from initialOwnership",
                           path.c_str(), count);
            return false;
        }

        const auto& regions = m_ctx->data->GetContinent().regions;
        for (size_t i = 0; i < count; ++i)
        {
            const int raw = owners[i].AsInt(0);
            FactionId owner =
                (raw >= 0 && raw < static_cast<int>(FactionId::COUNT)) ? static_cast<FactionId>(raw) : FactionId::None;
            if (i < regions.size() && regions[i].tier == "skyanchor")
                owner = regions[i].homeFaction; // homes never change hands
            m_state[i].owner = owner;
            m_state[i].capturing = FactionId::None;
            m_state[i].progress = 0.0f;
            m_state[i].contested = false;
        }

        const Value& dom = root["dominion"];
        m_domActive = dom.IsObject() && dom["active"].AsBool(false);
        if (m_domActive)
        {
            const int f = dom["faction"].AsInt(0);
            if (f >= 1 && f < static_cast<int>(FactionId::COUNT))
            {
                m_domFaction = static_cast<FactionId>(f);
                const double remaining = std::clamp(dom["remainingSec"].AsNumber(0.0), 0.0, 600.0);
                m_domEndsAt = m_time + remaining;
            }
            else
            {
                m_domActive = false;
            }
        }

        m_dirty = false;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] territory restored from %s (hash 0x%08X%s)", path.c_str(),
                       TerritoryHash(), m_domActive ? ", dominion hold" : "");
        return true;
    }

    bool TFRegionSystem::PersistNow()
    {
        if (!m_ctx || !m_ctx->IsAuthority() || m_state.empty())
            return false;

        using Spark::Json::Value;
        Value root = Value::MakeObject();
        root["version"] = Value(kSaveVersion);
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

        namespace fs = std::filesystem;
        const std::string path = SavePath();
        const std::string tmp = path + ".tmp";
        std::error_code ec;
        fs::create_directories(fs::path(path).parent_path(), ec);

        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f.is_open())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] territory save failed: cannot write %s", tmp.c_str());
                return false;
            }
            f << Spark::Json::StringifyPretty(root);
            if (!f.good())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] territory save failed: short write to %s", tmp.c_str());
                return false;
            }
        }

        fs::rename(tmp, path, ec); // atomic replace (MOVEFILE_REPLACE_EXISTING on Win)
        if (ec)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] territory save failed: rename to %s (%s)", path.c_str(),
                           ec.message().c_str());
            fs::remove(tmp, ec);
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
