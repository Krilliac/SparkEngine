/**
 * @file TFAlertSystemNet.cpp
 * @brief TFAlertSystem state view + wire plumbing: the TF_AlertState builder,
 *        the unified server/mirror view accessor, the reliable broadcast on
 *        the frozen 0x5470 id, client mirror handler lifecycle and the
 *        late-joiner burst / leaver sweep poll. Split from TFAlertSystem.cpp
 *        (same class, split per the repo file-size rules — mirrors the
 *        TFTravelSystem split).
 */
#include "World/TFAlertSystem.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // State view + wire
    // ---------------------------------------------------------------------------

    void TFAlertSystem::BuildState(TF_AlertState& out) const
    {
        out = TF_AlertState{};
        out.phase = static_cast<uint8_t>(m_phase);
        out.type = static_cast<uint8_t>(m_type);
        out.winner = static_cast<uint8_t>(m_winner);
        out.regionId = m_target;
        for (size_t i = 0; i < m_scores.size(); ++i)
            out.score[i] = m_scores[i];
        out.secondsLeft = std::max(m_secondsLeft, 0.0f);
    }

    void TFAlertSystem::GetView(TF_AlertState& out) const
    {
        if (m_ctx && m_ctx->IsAuthority())
        {
            BuildState(out);
            return;
        }
        if (m_mirrorValid)
        {
            out = m_mirror;
            return;
        }
        out = TF_AlertState{};
        out.phase = static_cast<uint8_t>(TFAlertPhase::Idle);
        out.regionId = kInvalidRegion;
    }

    void TFAlertSystem::BroadcastState()
    {
        TF_AlertState st{};
        BuildState(st);
        ++m_statesSent;
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
        {
            for (const auto& [id, info] : nm.GetClients())
            {
                if (info.state == Spark::Net::ConnectionState::Connected)
                    SendWire(id, st);
            }
        }
#endif
        // Listen host / standalone local player: RenderUI reads the server
        // state directly through GetView — no loopback mirror write needed.
    }

    void TFAlertSystem::ClientHandleState(const TF_AlertState& st)
    {
        m_mirror = st;
        m_mirrorValid = true;
        ++m_statesRx;
    }

#ifdef ENABLE_NETWORKING

    bool TFAlertSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFAlertSystem::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgAlertState),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_AlertState))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_AlertState st{};
                               std::memcpy(&st, m.payload.data(), sizeof(st));
                               ClientHandleState(st);
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] alert mirror handler registered");
    }

    void TFAlertSystem::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgAlertState),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFAlertSystem::SendWire(PlayerId target, const TF_AlertState& st)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(kTFMsgAlertState);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(sizeof(st));
        std::memcpy(msg.payload.data(), &st, sizeof(st));
        nm.SendToClient(target, msg);
    }

    void TFAlertSystem::PollJoins()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;

        // Late-joiner burst: a fresh client gets the current state once while
        // an alert is running or its end splash is showing (idle mirrors are
        // the client default — no traffic needed).
        for (const auto& [id, info] : nm.GetClients())
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            if (m_phase != TFAlertPhase::Idle)
            {
                TF_AlertState st{};
                BuildState(st);
                SendWire(id, st);
                ++m_statesSent;
            }
        }

        // Leaver sweep: recycled-PlayerId hygiene for the participant sets
        // (the TFMedalSystem PollJoinsLeaves precedent).
        std::erase_if(m_knownClients,
                      [this, &nm](PlayerId id)
                      {
                          if (nm.GetClients().contains(id))
                              return false;
                          for (auto& set : m_participants)
                              set.erase(id);
                          return true;
                      });
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
