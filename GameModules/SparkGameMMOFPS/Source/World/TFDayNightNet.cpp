/**
 * @file TFDayNightNet.cpp
 * @brief TFDayNight wire plumbing: the reliable TF_TimeOfDay broadcast on the
 *        frozen 0x5478 id, client mirror handler lifecycle, the reliable
 *        server->client send helper and the late-joiner burst / leaver sweep
 *        poll. Split from TFDayNight.cpp (same class, split per the repo
 *        file-size rules — mirrors the TFAlertSystem split).
 */
#include "World/TFDayNight.h"

#include "Utils/LogMacros.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    // ---------------------------------------------------------------------------
    // Wire
    // ---------------------------------------------------------------------------

    void TFDayNight::BroadcastTime()
    {
#ifdef ENABLE_NETWORKING
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server)
        {
            TF_TimeOfDay msg{};
            msg.dayFrac = m_dayFrac;
            msg.rate = kTFDayRatePerSec;
            for (const auto& [id, info] : nm.GetClients())
            {
                if (info.state == Spark::Net::ConnectionState::Connected)
                    SendWire(id, msg);
            }
        }
#endif
        // Listen host / standalone local player: visuals read the server clock
        // directly through GetView — no loopback mirror write needed.
    }

#ifdef ENABLE_NETWORKING

    bool TFDayNight::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return m_ctx->role == NetRole::Client && nm.IsInitialized() &&
               nm.GetRole() == Spark::Net::NetworkRole::Client &&
               nm.GetConnectionState() == Spark::Net::ConnectionState::Connected;
    }

    void TFDayNight::EnsureClientHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFMsgTimeOfDay),
                           [this](const NetworkMessage& m)
                           {
                               if (m.payload.size() != sizeof(TF_TimeOfDay))
                               {
                                   ++m_badPackets;
                                   return;
                               }
                               TF_TimeOfDay msg{};
                               std::memcpy(&msg, m.payload.data(), sizeof(msg));
                               msg.dayFrac = WrapFrac(msg.dayFrac);
                               msg.rate = std::clamp(msg.rate, 0.0f, 1.0f); // sanity: <= 1 day per second
                               m_mirror = msg;
                               m_mirrorValid = true;
                               ++m_syncsRx;
                           });
        m_clientHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] time-of-day mirror handler registered");
    }

    void TFDayNight::ReleaseClientHandlers()
    {
        // NetworkManager has no per-type removal; replace with a no-op so no
        // dangling `this` survives module shutdown (TFServerSim pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<Spark::Net::MessageType>(kTFMsgTimeOfDay),
                           [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFDayNight::SendWire(PlayerId target, const TF_TimeOfDay& msg)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage out;
        out.type = static_cast<Spark::Net::MessageType>(kTFMsgTimeOfDay);
        out.channel = Spark::Net::ChannelType::Reliable;
        out.payload.resize(sizeof(msg));
        std::memcpy(out.payload.data(), &msg, sizeof(msg));
        nm.SendToClient(target, out);
        ++m_syncsSent;
    }

    void TFDayNight::PollJoins()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;

        // Late-joiner burst: unlike alerts, EVERY fresh client needs the clock
        // immediately (the mirror default is only the boot time).
        for (const auto& [id, info] : nm.GetClients())
        {
            if (info.state != Spark::Net::ConnectionState::Connected || m_knownClients.contains(id))
                continue;
            m_knownClients.insert(id);
            TF_TimeOfDay msg{};
            msg.dayFrac = m_dayFrac;
            msg.rate = kTFDayRatePerSec;
            SendWire(id, msg);
        }

        // Leaver sweep: recycled-PlayerId hygiene.
        std::erase_if(m_knownClients, [&nm](PlayerId id) { return !nm.GetClients().contains(id); });
    }

#endif // ENABLE_NETWORKING

} // namespace Terrafront
