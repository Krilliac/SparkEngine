/**
 * @file TFTravelSystemClient.cpp
 * @brief TFTravelSystem client half: local pawn/terminal helpers, the menu
 *        open/close mouse handoff, travel/info requests, travel replies and
 *        the server-authoritative continent-hop request/apply flow. Split
 *        from TFTravelSystem.cpp (same class, split per the repo file-size
 *        rules — mirrors the TFVehicleSystem split); shared internals live in
 *        TFTravelSystemInternal.h.
 */
#include "World/TFTravelSystem.h"

#include "Game/TFPlayerSystem.h"
#include "Game/TFUiSounds.h" // W10 audio-wave-2: terminal bleeps
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h" // W13 server-authoritative continent-hop: TF_ContinentHopRequest/Reply
#include "World/TFTravelSystemInternal.h"
#include "World/TFWorldSetup.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <algorithm>
#include <cstdio>

namespace Terrafront
{

    using namespace TravelDetail;

    PlayerId TFTravelSystem::LocalPlayerId() const
    {
        if (!m_ctx)
            return kInvalidPlayer;
        if (m_ctx->localPlayer != kInvalidPlayer)
            return m_ctx->localPlayer;
        return m_ctx->clientNet ? m_ctx->clientNet->LocalPlayerId() : kInvalidPlayer;
    }

    bool TFTravelSystem::LocalPawn(float outPos[3], bool& outAlive) const
    {
        outAlive = false;
        const PlayerId pid = LocalPlayerId();
        if (pid == kInvalidPlayer || !m_ctx || !m_ctx->players)
            return false;
        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(pid, pawn))
            return false;
        outPos[0] = pawn.pos[0];
        outPos[1] = pawn.pos[1];
        outPos[2] = pawn.pos[2];
        outAlive = pawn.alive;
        return true;
    }

    bool TFTravelSystem::NearTerminal(const float pawnPos[3], float radiusM) const
    {
        return DistSqXZ(pawnPos, kTFSanctuaryTerminalX, kTFSanctuaryTerminalZ) <= radiusM * radiusM;
    }

    void TFTravelSystem::SetMenuOpen(bool open)
    {
        if (m_menuOpen == open)
            return;
        m_menuOpen = open;
        // TFMapScreen/vehicle-shop pattern: an open menu owns the mouse.
        InputManager* input = (m_ctx && m_ctx->engine) ? m_ctx->engine->GetInput() : nullptr;
        if (!input)
            return;
        if (open)
        {
            if (input->IsMouseCaptured())
                input->CaptureMouse(false);
        }
        else
        {
            float pos[3];
            bool alive = false;
            if (LocalPawn(pos, alive) && alive)
                input->CaptureMouse(true);
        }
    }

    void TFTravelSystem::ClientRequestTravel(uint8_t destMapId)
    {
        if (!m_ctx)
            return;
        TF_TravelRequest req{};
        req.mapId = destMapId;
        if (m_ctx->IsAuthority())
        {
            // Direct authority path (vehicle ClientRequest* precedent). The
            // enter-world gate is applied inside ServerHandleTravel.
            ServerHandleTravel(LocalPlayerId(), req);
            return;
        }
        if (m_ctx->clientNet && m_ctx->clientNet->IsConnected())
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFTravelMsg_Request), &req, sizeof(req));
    }

    void TFTravelSystem::ClientRequestContinentHop(const ContinentMeta& target)
    {
        // multimap-plumbing lane (W13) + server-authoritative follow-up: see
        // docs/TERRAFRONT_MULTIMAP.md §2.2 for the design this implements.
        // Only a genuine remote client can hop — the local authority role
        // (ListenHost/DedicatedServer/Standalone loopback) already IS the
        // server for the active continent, so there is nothing to reconnect.
        if (!m_ctx || !m_ctx->world)
            return;
        if (m_ctx->role != NetRole::Client)
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Only a connected client can travel servers");
            return;
        }
        if (target.mapId < 0)
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Unknown continent");
            return;
        }
        if (!m_ctx->clientNet || !m_ctx->clientNet->IsConnected())
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Not connected to a server");
            return;
        }

        // Server-authoritative redirect (§2.2): ask the server THIS client is
        // CURRENTLY connected to — the trust boundary, same as every other
        // TFMsg — to resolve `target.mapId` against ITS OWN registry
        // (TFTravelSystem::LookupContinentEndpoint on that process) rather
        // than trusting this client's local continents.json copy (or a
        // locally-observed LAN beacon) for the actual endpoint; those are now
        // display hints only (see RenderUI). The reply drives
        // ApplyPendingContinentHop on a later Update() tick — see
        // OnNetContinentHopReply.
        m_hopRequestMapId = target.mapId;
        m_hopRequestName = target.name;
        TF_ContinentHopRequest req{};
        req.mapId = static_cast<uint8_t>(target.mapId);
        m_ctx->clientNet->SendMsg(TFMsg::ContinentHopRequest, &req, sizeof(req));
        std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Requesting %s server address...", target.name.c_str());
    }

    bool TFTravelSystem::LookupContinentEndpoint(uint8_t mapId, std::string& outHost, uint16_t& outPort) const
    {
        for (const ContinentMeta& c : m_continentList)
        {
            if (c.mapId != static_cast<int>(mapId))
                continue;
            if (c.host.empty() || c.port == 0)
                return false; // registered continent, but no endpoint configured
            outHost = c.host;
            outPort = c.port;
            return true;
        }
        return false; // unregistered mapId
    }

    void TFTravelSystem::ApplyPendingContinentHop()
    {
        const std::string name = m_hopRequestName.empty() ? std::string("that continent") : m_hopRequestName;
        const bool ok = m_hopReplyOk;
        const std::string host = m_hopReplyHost;
        const uint16_t port = m_hopReplyPort;
        m_hopReplyPending = false;
        m_hopRequestMapId = -1;
        m_hopRequestName.clear();

        if (!ok)
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "No server hosting %s", name.c_str());
            TFUiSounds_Play(m_ctx, TFUiBleep::Deny); // W10 audio-wave-2
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("[TF] continent hop refused: no server for ") +
                                                           name);
            return;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] continent hop -> %s (%s:%u, server-verified)", name.c_str(),
                       host.c_str(), static_cast<unsigned>(port));

        // Best-effort teardown of the CURRENT connection before dialing the new
        // one. TFClientNet::Disconnect() resets TF-level client state + ctx.role
        // but does not itself touch the NetworkManager socket; the raw
        // NetworkManager::Disconnect() call mirrors the tf_disconnect console
        // command (TFCommands.cpp) which drops the socket but is tagged "TF-W2"
        // there as not yet routed through a clean TFWorldSetup teardown API.
        // Both are best-effort — see docs/TERRAFRONT_MULTIMAP.md "known gaps"
        // for the residual risk (client visuals don't reload for the new
        // continent's scene/collision).
        if (m_ctx->clientNet)
            m_ctx->clientNet->Disconnect();
#ifdef ENABLE_NETWORKING
        {
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.IsInitialized())
                nm.Disconnect();
        }
#endif

        if (!m_ctx->world || !m_ctx->world->Connect(host, port))
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Connect to %s failed (see log)", name.c_str());
            return;
        }
        std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Connecting to %s...", name.c_str());
        TFUiSounds_Play(m_ctx, TFUiBleep::Confirm);
        SetMenuOpen(false);
    }

    void TFTravelSystem::ClientRequestInfo()
    {
        if (!m_ctx)
            return;
        m_infoRequestedAt = m_clock;
        if (m_ctx->IsAuthority())
        {
            TF_ContinentInfo info;
            ServerBuildContinentInfo(info);
            ApplyContinentInfo(info);
            return;
        }
        if (m_ctx->clientNet && m_ctx->clientNet->IsConnected())
            m_ctx->clientNet->SendMsg(static_cast<TFMsg>(kTFTravelMsg_InfoRequest), nullptr, 0);
    }

    void TFTravelSystem::ApplyTravelReply(const TF_TravelReply& rep)
    {
        static const char* kReasons[] = {"ok",          "unknown map",   "not entered world",
                                         "not alive",   "already there", "not at the terminal",
                                         "server error"};
        const size_t r = std::min<size_t>(rep.reason, 6);
        if (rep.accepted)
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Traveling to %s...", TFTravel_MapName(rep.mapId));
            TFUiSounds_Play(m_ctx, TFUiBleep::Confirm); // W10 audio-wave-2
            SetMenuOpen(false);
            Spark::SimpleConsole::GetInstance().LogInfo(std::string("[TF] travel accepted -> ") +
                                                        TFTravel_MapName(rep.mapId));
        }
        else
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Travel refused: %s", kReasons[r]);
            TFUiSounds_Play(m_ctx, TFUiBleep::Deny); // W10 audio-wave-2
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("[TF] travel refused: ") + kReasons[r]);
        }
    }

    void TFTravelSystem::ApplyContinentInfo(const TF_ContinentInfo& info)
    {
        m_lastInfo = info;
        m_hasInfo = true;
    }

} // namespace Terrafront
