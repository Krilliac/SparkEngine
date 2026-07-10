/**
 * @file TFTravelSystem.cpp
 * @brief Continent/sanctuary travel — server placement + validation half and
 *        client terminal UX half (one file, TFVehicleSystem precedent).
 *
 * See TFTravelSystem.h / TFSanctuaryZone.h for the architecture note (single
 * shared sim, positional mapId). Server rules enforced here:
 *  - first spawn of an entered-world session lands on a sanctuary pad
 *    (deferred one fixed tick — EvPlayerSpawned fires synchronously INSIDE
 *    the spawn handlers, which overwrite MoveState after the event returns,
 *    so an immediate TeleportPawn would be clobbered);
 *  - sanctuary -> continent requires standing at the travel terminal;
 *  - continent -> sanctuary is an unrestricted recall (PS2 style);
 *  - every travel teleports through TFServerSim::TeleportPawn (the one
 *    authoritative move primitive; prediction reconciles the snap client-side).
 */
#include "World/TFTravelSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFRedeployRules.h" // ui-map-keys seam: SetExtraRule (sanctioned extension point)
#include "Net/TFClientNet.h"
#include "Net/TFRedeployProtocol.h" // kTFRedeployBlocked reason code
#include "Net/TFServerSim.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"
#include "World/TFRegionSystem.h"
#include "World/TFWorldSetup.h"

#include "Input/InputManager.h"
#include "Spark/IEngineContext.h"
#include "Utils/JsonUtils.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#ifdef SPARK_HAS_IMGUI
#include "UI/TFUiCommon.h"
#include <imgui.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace Terrafront
{

    namespace
    {

        constexpr float kInteractDebounceSec = 0.25f;
        constexpr float kMenuAutoCloseM = 20.0f; ///< walk away -> menu closes
        constexpr float kServerUseSlackM = 2.0f; ///< latency slack on the server-side terminal check
        constexpr float kPruneEverySec = 10.0f;  ///< stale mapId sweep cadence
        constexpr double kInfoRefreshSec = 2.0;  ///< menu population refresh cadence
        constexpr const char* kContinentsJson = "Assets/MMOFPS/Data/continents.json";

        float DistSqXZ(const float a[3], float bx, float bz)
        {
            const float dx = a[0] - bx;
            const float dz = a[2] - bz;
            return dx * dx + dz * dz;
        }

    } // namespace

    TFTravelSystem::TFTravelSystem() = default;
    TFTravelSystem::~TFTravelSystem()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFTravelSystem::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        m_ctx = &ctx;
        m_events = &events;

        events.Subscribe<EvPlayerSpawned>([this](const EvPlayerSpawned& ev) { OnPlayerSpawned(ev); });

        // Sanctuary redeploy veto — installed into the ui-map-keys lane's
        // sanctioned extension point (TFRedeployRules.h names this lane as the
        // extender). Deliberately CAPTURE-LESS: the rule outliving this system
        // can never dangle. Runs on BOTH sides after the baseline friendly +
        // non-contested checks, so this only adds the sanctuary rule.
        TFRedeployRules::SetExtraRule(
            [](const TFGameContext& ctx, PlayerId player, RegionId /*region*/) -> uint8_t
            {
                PawnInfo pawn{};
                if (ctx.players && ctx.players->GetPawnByPlayer(player, pawn) && pawn.alive &&
                    TFTravel_IsInSanctuary(pawn.pos[0], pawn.pos[2]))
                    return kTFRedeployBlocked; // use the travel terminal instead
                return kTFRedeployOk;
            });

        LoadContinentMeta();
        RegisterConsoleCommands();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] TFTravelSystem initialized (sanctuary pad %.0f/%.0f, terminal %.0f/%.0f)",
                       kTFSanctuaryCenterX, kTFSanctuaryCenterZ, kTFSanctuaryTerminalX, kTFSanctuaryTerminalZ);
        return true;
    }

    void TFTravelSystem::Shutdown()
    {
        if (!m_initialized)
            return;
        TFRedeployRules::SetExtraRule({}); // capture-less, but clear for symmetry
#ifdef ENABLE_NETWORKING
        if (m_serverHandlers)
            ServerReleaseNetHandlers();
        if (m_clientHandlers)
            ClientReleaseNetHandlers();
#endif
        m_mapOf.clear();
        m_pendingPlace.clear();
        m_initialized = false;
    }

    // ---------------------------------------------------------------------------
    // Data / console
    // ---------------------------------------------------------------------------

    void TFTravelSystem::LoadContinentMeta()
    {
        // Display metadata ONLY (names/blurb). Geometry/zone constants are
        // compile-time in TFSanctuaryZone.h so terrain/damage determinism never
        // depends on a data file load order (see the header note).
        std::ifstream f(kContinentsJson, std::ios::binary);
        if (!f.is_open())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] %s missing; using built-in continent names",
                           kContinentsJson);
            return;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        const Spark::Json::Value root = Spark::Json::Parse(ss.str());
        if (!root.IsObject() || !root.HasKey("continents") || !root["continents"].IsArray())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] %s malformed; using built-in continent names",
                           kContinentsJson);
            return;
        }
        const Spark::Json::Value& arr = root["continents"];
        for (size_t i = 0; i < arr.Size(); ++i)
        {
            const Spark::Json::Value& o = arr[i];
            if (!o.IsObject() || !o.HasKey("mapId"))
                continue;
            const int mapId = o["mapId"].AsInt(-1);
            const std::string name = o.HasKey("name") ? o["name"].AsString("") : std::string();
            if (name.empty())
                continue;
            if (mapId == kTFMapSanctuary)
                m_sanctuaryDisplayName = name;
            else if (mapId == kTFMapCindralWastes)
            {
                m_continentDisplayName = name;
                if (o.HasKey("blurb"))
                    m_continentBlurb = o["blurb"].AsString(m_continentBlurb);
            }
        }
    }

    void TFTravelSystem::RegisterConsoleCommands()
    {
        auto& console = Spark::SimpleConsole::GetInstance();

        if (!console.HasCommand("tf_travel"))
        {
            console.RegisterCommand(
                "tf_travel",
                [this](const std::vector<std::string>& args) -> std::string
                {
                    if (!m_initialized)
                        return "[TF] travel system not ready";
                    if (args.empty())
                        return "usage: tf_travel <sanctuary|cindral>";
                    const std::string& a = args[0];
                    uint8_t dest;
                    if (a == "sanctuary" || a == "haven" || a == "0")
                        dest = kTFMapSanctuary;
                    else if (a == "cindral" || a == "wastes" || a == "continent" || a == "1")
                        dest = kTFMapCindralWastes;
                    else
                        return "usage: tf_travel <sanctuary|cindral>";
                    ClientRequestTravel(dest);
                    return std::string("[TF] travel requested -> ") + TFTravel_MapName(dest) +
                           " (server validates; see reply in log/menu)";
                },
                "Request travel to a map (server validates: alive, entered world, terminal proximity for "
                "sanctuary departures)",
                "TERRAFRONT", "tf_travel <sanctuary|cindral>");
        }

        if (!console.HasCommand("tf_travel_debug"))
        {
            console.RegisterCommand(
                "tf_travel_debug",
                [this](const std::vector<std::string>&) -> std::string
                {
                    ToggleDebugUI();
                    return "[TF] travel debug panel toggled (needs tf_debug panels on)";
                },
                "Toggle the TF Travel debug panel", "TERRAFRONT", "tf_travel_debug");
        }
    }

    // ---------------------------------------------------------------------------
    // Cross-lane surface
    // ---------------------------------------------------------------------------

    uint8_t TFTravelSystem::ServerMapOf(PlayerId player) const
    {
        const auto it = m_mapOf.find(player);
        return it != m_mapOf.end() ? it->second : kTFMapSanctuary;
    }

    uint8_t TFTravelSystem::LocalMapId() const
    {
        float pos[3];
        bool alive = false;
        if (!LocalPawn(pos, alive))
            return kTFMapCindralWastes;
        return TFTravel_MapIdAt(pos[0], pos[2]);
    }

    bool TFTravelSystem::CanRedeployTo(RegionId region) const
    {
        if (!m_ctx || !m_ctx->regions)
            return false;

        // In the sanctuary there is no redeploy at all — the terminal is the way out.
        float pos[3];
        bool alive = false;
        if (LocalPawn(pos, alive) && TFTravel_IsInSanctuary(pos[0], pos[2]))
            return false;

        // On the continent: friendly AND non-contested regions only.
        const FactionId myFaction = m_ctx->localFaction;
        if (myFaction == FactionId::None)
            return false;
        if (m_ctx->regions->OwnerOf(region) != myFaction)
            return false;
        FactionId capturing = FactionId::None;
        bool contested = false;
        const float progress = m_ctx->regions->CaptureProgress(region, capturing, contested);
        if (contested || (capturing != FactionId::None && progress > 0.0f))
            return false;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Server half
    // ---------------------------------------------------------------------------

    void TFTravelSystem::OnPlayerSpawned(const EvPlayerSpawned& ev)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->serverSim)
            return;

        // Only sessions that passed the enter-world gate get sanctuary placement.
        // Bots (kTFBotIdBase ids) and pre-gate connections are never in
        // m_enteredWorld, so they spawn wherever the spawn pipeline put them.
        // (Builds without ENABLE_NETWORKING never populate the gate — the
        // sanctuary flow simply stays dormant there.)
        if (!m_ctx->serverSim->IsEnteredWorld(ev.player))
            return;

        const auto it = m_mapOf.try_emplace(ev.player, kTFMapSanctuary).first;
        if (it->second != kTFMapSanctuary)
            return; // continent resident: normal skyanchor/region spawn stands

        // DO NOT teleport here: this event fires synchronously inside the spawn
        // handlers, which write MoveState AFTER firing it (TFServerSim
        // HandleSpawnRequest seeds m_move[sender] post-ServerSpawnPawn). Queue
        // for the next authoritative fixed tick instead.
        m_pendingPlace.insert(ev.player);
    }

    void TFTravelSystem::ServerPlacePending()
    {
        if (m_pendingPlace.empty() || !m_ctx->serverSim)
            return;
        for (const PlayerId pid : m_pendingPlace)
        {
            if (!m_ctx->serverSim->IsPlayerAlive(pid))
                continue; // died/despawned between event and tick
            float pos[3];
            ServerSanctuaryPadOf(pid, pos);
            m_ctx->serverSim->TeleportPawn(pid, pos[0], pos[1], pos[2]);
            ++m_placements;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u placed in %s (%.0f, %.0f)", pid,
                           m_sanctuaryDisplayName.c_str(), pos[0], pos[2]);
        }
        m_pendingPlace.clear();
    }

    void TFTravelSystem::ServerPruneStale(float dt)
    {
        m_pruneAccum += dt;
        if (m_pruneAccum < kPruneEverySec || !m_ctx->serverSim)
            return;
        m_pruneAccum = 0.0f;
        for (auto it = m_mapOf.begin(); it != m_mapOf.end();)
        {
            if (!m_ctx->serverSim->IsEnteredWorld(it->first) && !m_ctx->serverSim->IsPlayerAlive(it->first))
                it = m_mapOf.erase(it);
            else
                ++it;
        }
    }

    void TFTravelSystem::ServerSanctuaryPadOf(PlayerId player, float outPos[3]) const
    {
        const uint32_t pad = player % kTFSanctuarySpawnPadCount;
        outPos[0] = kTFSanctuarySpawnPads[pad][0];
        outPos[2] = kTFSanctuarySpawnPads[pad][1];
        outPos[1] = m_ctx->world ? m_ctx->world->TerrainHeightAt(outPos[0], outPos[2]) : kTFSanctuaryPadY;
    }

    bool TFTravelSystem::ServerWarpgateOf(FactionId faction, float outPos[3]) const
    {
        if (!m_ctx->data || !m_ctx->data->IsLoaded())
            return false;
        for (const RegionDef& r : m_ctx->data->GetContinent().regions)
        {
            if (r.tier != "skyanchor" || r.homeFaction != faction)
                continue;
            outPos[0] = r.spawns.empty() ? r.centerX : (*r.spawns.begin())[0];
            outPos[2] = r.spawns.empty() ? r.centerZ : (*r.spawns.begin())[1];
            outPos[1] = m_ctx->world ? m_ctx->world->TerrainHeightAt(outPos[0], outPos[2]) : 0.0f;
            return true;
        }
        return false;
    }

    void TFTravelSystem::ServerHandleTravel(PlayerId sender, const TF_TravelRequest& req)
    {
        if (!m_ctx || !m_ctx->IsAuthority() || !m_ctx->serverSim)
            return;

        TF_TravelReply rep{};
        rep.accepted = 0;
        rep.mapId = req.mapId;

        PawnInfo pawn{};
        const bool havePawn = m_ctx->players && m_ctx->players->GetPawnByPlayer(sender, pawn) && pawn.alive;

        if (req.mapId >= kTFMapCount)
            rep.reason = static_cast<uint8_t>(TFTravelErr::BadMap);
#ifdef ENABLE_NETWORKING
        // Same enter-world gate every client-originated gameplay message is
        // held to (TFServerSim::RouteClientMessage choke point / the vehicle
        // authority-path mirror). Gate exists only with networking built in.
        else if (!m_ctx->serverSim->IsEnteredWorld(sender))
            rep.reason = static_cast<uint8_t>(TFTravelErr::NotEntered);
#endif
        else if (!m_ctx->serverSim->IsPlayerAlive(sender) || !havePawn)
            rep.reason = static_cast<uint8_t>(TFTravelErr::NotAlive);
        else
        {
            const uint8_t cur = TFTravel_MapIdAt(pawn.pos[0], pawn.pos[2]);
            if (cur == req.mapId)
                rep.reason = static_cast<uint8_t>(TFTravelErr::AlreadyThere);
            else if (cur == kTFMapSanctuary &&
                     DistSqXZ(pawn.pos, kTFSanctuaryTerminalX, kTFSanctuaryTerminalZ) >
                         (kTFTravelTerminalUseM + kServerUseSlackM) * (kTFTravelTerminalUseM + kServerUseSlackM))
            {
                // Leaving the sanctuary requires standing at the terminal.
                // (Continent -> sanctuary is an unrestricted recall.)
                rep.reason = static_cast<uint8_t>(TFTravelErr::NoTerminal);
            }
            else
            {
                float dest[3]{0.0f, 0.0f, 0.0f};
                bool haveDest = false;
                if (req.mapId == kTFMapCindralWastes)
                {
                    const FactionId faction = m_ctx->serverSim->GetPlayerFaction(sender);
                    haveDest = ServerWarpgateOf(faction, dest);
                }
                else
                {
                    ServerSanctuaryPadOf(sender, dest);
                    haveDest = true;
                }

                if (!haveDest)
                    rep.reason = static_cast<uint8_t>(TFTravelErr::ServerError);
                else
                {
                    m_ctx->serverSim->TeleportPawn(sender, dest[0], dest[1], dest[2]);
                    m_mapOf[sender] = req.mapId;
                    ++m_travels;
                    rep.accepted = 1;
                    rep.reason = static_cast<uint8_t>(TFTravelErr::Ok);
                    rep.posX = dest[0];
                    rep.posY = dest[1];
                    rep.posZ = dest[2];
                    SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u traveled to %s (%.0f, %.0f)", sender,
                                   TFTravel_MapName(req.mapId), dest[0], dest[2]);
                }
            }
        }

        if (!rep.accepted)
            ++m_travelsRejected;

        // Reply: in-process for the listen-host/standalone local player (it has
        // no NetworkManager socket — the TFServerSim::SendToPlayer precedent),
        // over the wire otherwise.
        if (sender == m_ctx->localPlayer)
        {
            ApplyTravelReply(rep);
            return;
        }
#ifdef ENABLE_NETWORKING
        ServerSendTo(sender, kTFTravelMsg_Reply, &rep, sizeof(rep));
#endif
    }

    void TFTravelSystem::ServerBuildContinentInfo(TF_ContinentInfo& out) const
    {
        out = TF_ContinentInfo{};
        out.mapId = kTFMapCindralWastes;
        if (m_ctx->players)
        {
            m_ctx->players->ForEachAlivePawn(
                [&](const PawnInfo& p)
                {
                    const size_t f = static_cast<size_t>(p.faction);
                    if (f >= 4)
                        return;
                    if (TFTravel_MapIdAt(p.pos[0], p.pos[2]) == kTFMapCindralWastes && out.pop[f] < 0xFFFF)
                        ++out.pop[f];
                });
        }
        if (m_ctx->regions)
        {
            out.held[static_cast<size_t>(FactionId::MRA)] =
                static_cast<uint16_t>(m_ctx->regions->RegionsHeld(FactionId::MRA));
            out.held[static_cast<size_t>(FactionId::AUC)] =
                static_cast<uint16_t>(m_ctx->regions->RegionsHeld(FactionId::AUC));
            out.held[static_cast<size_t>(FactionId::HLX)] =
                static_cast<uint16_t>(m_ctx->regions->RegionsHeld(FactionId::HLX));
        }
    }

    // ---------------------------------------------------------------------------
    // Client half
    // ---------------------------------------------------------------------------

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
            SetMenuOpen(false);
            Spark::SimpleConsole::GetInstance().LogInfo(std::string("[TF] travel accepted -> ") +
                                                        TFTravel_MapName(rep.mapId));
        }
        else
        {
            std::snprintf(m_lastTravelMsg, sizeof(m_lastTravelMsg), "Travel refused: %s", kReasons[r]);
            Spark::SimpleConsole::GetInstance().LogWarning(std::string("[TF] travel refused: ") + kReasons[r]);
        }
    }

    void TFTravelSystem::ApplyContinentInfo(const TF_ContinentInfo& info)
    {
        m_lastInfo = info;
        m_hasInfo = true;
    }

    // ---------------------------------------------------------------------------
    // Networking (own channel, TFVehicleNet precedent)
    // ---------------------------------------------------------------------------

#ifdef ENABLE_NETWORKING

    bool TFTravelSystem::ServerNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Server && m_ctx->IsAuthority();
    }

    bool TFTravelSystem::ClientNetActive() const
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        return nm.IsInitialized() && nm.GetRole() == Spark::Net::NetworkRole::Client;
    }

    void TFTravelSystem::ServerEnsureNetHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_Request), [this](const NetworkMessage& m)
                           { ServerHandleTravelPacket(m.senderID, m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_InfoRequest), [this](const NetworkMessage& m)
                           { ServerHandleInfoRequestPacket(m.senderID, m.payload.data(), m.payload.size()); });
        m_serverHandlers = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] travel server net handlers registered");
    }

    void TFTravelSystem::ServerReleaseNetHandlers()
    {
        // No per-type removal in NetworkManager; overwrite with no-ops so no
        // dangling `this` survives shutdown (module-wide pattern).
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFTravelMsg_Request, kTFTravelMsg_InfoRequest})
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_serverHandlers = false;
    }

    void TFTravelSystem::ClientEnsureNetHandlers()
    {
        using Spark::Net::MessageType;
        using Spark::Net::NetworkMessage;
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_Reply),
                           [this](const NetworkMessage& m) { OnNetTravelReply(m.payload.data(), m.payload.size()); });
        nm.RegisterHandler(static_cast<MessageType>(kTFTravelMsg_Info),
                           [this](const NetworkMessage& m) { OnNetContinentInfo(m.payload.data(), m.payload.size()); });
        m_clientHandlers = true;
    }

    void TFTravelSystem::ClientReleaseNetHandlers()
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        for (uint16_t id : {kTFTravelMsg_Reply, kTFTravelMsg_Info})
            nm.RegisterHandler(static_cast<Spark::Net::MessageType>(id), [](const Spark::Net::NetworkMessage&) {});
        m_clientHandlers = false;
    }

    void TFTravelSystem::ServerHandleTravelPacket(PlayerId sender, const void* data, size_t size)
    {
        if (size != sizeof(TF_TravelRequest) || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_TravelRequest req;
        std::memcpy(&req, data, sizeof(req));
        ServerHandleTravel(sender, req);
    }

    void TFTravelSystem::ServerHandleInfoRequestPacket(PlayerId sender, const void* data, size_t size)
    {
        (void)data;
        if (size != 0 || sender == Spark::Net::INVALID_CLIENT)
        {
            ++m_badPackets;
            return;
        }
        TF_ContinentInfo info;
        ServerBuildContinentInfo(info);
        ServerSendTo(sender, kTFTravelMsg_Info, &info, sizeof(info));
    }

    void TFTravelSystem::ServerSendTo(PlayerId player, uint16_t msgId, const void* payload, size_t size)
    {
        auto& nm = Spark::Net::NetworkManager::GetInstance();
        if (!nm.IsInitialized() || nm.GetRole() != Spark::Net::NetworkRole::Server)
            return;
        Spark::Net::NetworkMessage msg;
        msg.type = static_cast<Spark::Net::MessageType>(msgId);
        msg.channel = Spark::Net::ChannelType::Reliable;
        msg.payload.resize(size);
        if (size > 0)
            std::memcpy(msg.payload.data(), payload, size);
        nm.SendToClient(player, msg);
    }

    void TFTravelSystem::OnNetTravelReply(const void* data, size_t size)
    {
        if (size != sizeof(TF_TravelReply))
        {
            ++m_badPackets;
            return;
        }
        TF_TravelReply rep;
        std::memcpy(&rep, data, sizeof(rep));
        ApplyTravelReply(rep);
    }

    void TFTravelSystem::OnNetContinentInfo(const void* data, size_t size)
    {
        if (size != sizeof(TF_ContinentInfo))
        {
            ++m_badPackets;
            return;
        }
        TF_ContinentInfo info;
        std::memcpy(&info, data, sizeof(info));
        ApplyContinentInfo(info);
    }

#endif // ENABLE_NETWORKING

    // ---------------------------------------------------------------------------
    // Frame driving
    // ---------------------------------------------------------------------------

    void TFTravelSystem::Update(float deltaTime)
    {
        if (!m_initialized || !m_ctx)
            return;
        m_clock += deltaTime;
        m_interactDebounce = std::max(0.0f, m_interactDebounce - deltaTime);

#ifdef ENABLE_NETWORKING
        // Late registration polls (region-system pattern: roles appear after
        // Initialize, once tf_host/tf_connect runs).
        const bool serverUp = ServerNetActive();
        if (serverUp && !m_serverHandlers)
            ServerEnsureNetHandlers();
        else if (!serverUp && m_serverHandlers)
            ServerReleaseNetHandlers();
        const bool clientUp = ClientNetActive();
        if (clientUp && !m_clientHandlers)
            ClientEnsureNetHandlers();
        else if (!clientUp && m_clientHandlers)
            ClientReleaseNetHandlers();
#endif

        if (m_ctx->IsAuthority())
            ServerPruneStale(deltaTime);

        // ---- client-side terminal UX (all roles with a local player) ----------
        m_nearTerminal = false;
        if (!m_ctx->HasLocalPlayer())
            return;
        // Fullscreen UIs own the keys (vehicle-shop pattern).
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
        {
            SetMenuOpen(false);
            return;
        }
        InputManager* input = m_ctx->engine ? m_ctx->engine->GetInput() : nullptr;
        float pos[3];
        bool alive = false;
        if (!input || !LocalPawn(pos, alive) || !alive)
        {
            SetMenuOpen(false);
            return;
        }

        // NOTE: not named `near` — windef.h #defines near/far on Windows.
        const bool nearTerm = NearTerminal(pos, m_menuOpen ? kMenuAutoCloseM : kTFTravelTerminalUseM);
        if (nearTerm)
        {
            m_nearTerminal = true;
            if (input->WasKeyPressed('E') && m_interactDebounce <= 0.0f)
            {
                m_interactDebounce = kInteractDebounceSec;
                SetMenuOpen(!m_menuOpen);
                if (m_menuOpen)
                    ClientRequestInfo();
            }
            // Periodic population refresh while the menu is up.
            if (m_menuOpen && m_clock - m_infoRequestedAt >= kInfoRefreshSec)
                ClientRequestInfo();
        }
        else
        {
            SetMenuOpen(false);
        }
    }

    void TFTravelSystem::FixedUpdate(float fixedDeltaTime)
    {
        (void)fixedDeltaTime;
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;
        ServerPlacePending();
    }

    // ---------------------------------------------------------------------------
    // UI
    // ---------------------------------------------------------------------------

#ifdef SPARK_HAS_IMGUI

    void TFTravelSystem::RenderUI()
    {
        if (!m_initialized || !m_ctx || !m_ctx->HasLocalPlayer())
            return;
        if ((m_ctx->map && m_ctx->map->IsOpen()) || (m_ctx->spawnUI && m_ctx->spawnUI->IsOpen()))
            return;

        float pos[3];
        bool alive = false;
        if (!LocalPawn(pos, alive) || !alive)
            return;

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 promptAt(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.58f);

        if (!m_menuOpen)
        {
            if (m_nearTerminal)
                TFUi::AddTextCentered(fg, 17.0f, promptAt, IM_COL32(230, 230, 230, 220), "[E] Travel terminal");
            // Ambient sanctuary hint so new players know where they are.
            if (TFTravel_IsInSanctuary(pos[0], pos[2]))
            {
                char hint[96];
                std::snprintf(hint, sizeof(hint), "%s  -  safe zone. Use the travel terminal to deploy.",
                              m_sanctuaryDisplayName.c_str());
                TFUi::AddTextCentered(fg, 14.0f, ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + 26.0f),
                                      IM_COL32(180, 220, 255, 200), hint);
            }
            return;
        }

        // ---- continent select menu -------------------------------------------
        const float w = 420.0f, h = 240.0f;
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - w) * 0.5f, vp->Pos.y + (vp->Size.y - h) * 0.5f),
                                ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::Begin("Travel Terminal##tf_travel", &open,
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings))
        {
            ImGui::TextUnformatted("Select destination:");
            ImGui::Separator();

            ImGui::Text("%s", m_continentDisplayName.c_str());
            ImGui::TextDisabled("%s", m_continentBlurb.c_str());

            // Territory summary straight off the region mirror (works on every role).
            if (m_ctx->regions)
            {
                const uint32_t total = m_ctx->regions->RegionCount();
                ImGui::Text("Territory: ");
                for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX})
                {
                    float c[4];
                    FactionColor(f, c);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "%s %u/%u", FactionTag(f),
                                       m_ctx->regions->RegionsHeld(f), total);
                }
            }
            // Population from the server summary (authority builds it locally).
            if (m_hasInfo)
            {
                ImGui::Text("Population: ");
                for (FactionId f : {FactionId::MRA, FactionId::AUC, FactionId::HLX})
                {
                    float c[4];
                    FactionColor(f, c);
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(c[0], c[1], c[2], 1.0f), "%s %u", FactionTag(f),
                                       m_lastInfo.pop[static_cast<size_t>(f)]);
                }
            }
            else
            {
                ImGui::TextDisabled("Population: (querying...)");
            }

            ImGui::Spacing();
            const bool inSanctuary = TFTravel_IsInSanctuary(pos[0], pos[2]);
            ImGui::BeginDisabled(!inSanctuary);
            if (ImGui::Button("Deploy to Cindral Wastes", ImVec2(-1.0f, 0.0f)))
                ClientRequestTravel(kTFMapCindralWastes);
            ImGui::EndDisabled();
            if (!inSanctuary)
                ImGui::TextDisabled("(already deployed - use the map to redeploy)");

            if (m_lastTravelMsg[0] != '\0')
            {
                ImGui::Separator();
                ImGui::TextWrapped("%s", m_lastTravelMsg);
            }
            ImGui::Separator();
            ImGui::TextDisabled("[E] close");
        }
        ImGui::End();
        if (!open)
            SetMenuOpen(false);
    }

    void TFTravelSystem::RenderDebugUI()
    {
        if (!m_showDebug)
            return;
        if (ImGui::Begin("TF Travel", &m_showDebug))
        {
            ImGui::Text("local mapId     : %u (%s)", LocalMapId(), TFTravel_MapName(LocalMapId()));
            ImGui::Text("tracked players : %zu   pending placements: %zu", m_mapOf.size(), m_pendingPlace.size());
            ImGui::Text("travels         : %u (rejected %u)   sanctuary placements: %u", m_travels, m_travelsRejected,
                        m_placements);
            ImGui::Text("bad packets     : %u", m_badPackets);
            ImGui::Text("near terminal   : %s   menu: %s", m_nearTerminal ? "yes" : "no",
                        m_menuOpen ? "open" : "closed");
            if (m_hasInfo)
                ImGui::Text("last info       : pop MRA %u / AUC %u / HLX %u", m_lastInfo.pop[1], m_lastInfo.pop[2],
                            m_lastInfo.pop[3]);
            ImGui::Separator();
            for (const auto& [pid, mapId] : m_mapOf)
                ImGui::Text("player %u -> %s", pid, TFTravel_MapName(mapId));
        }
        ImGui::End();
    }

#else // !SPARK_HAS_IMGUI — headless builds keep the state machine, drop the pixels

    void TFTravelSystem::RenderUI() {}
    void TFTravelSystem::RenderDebugUI() {}

#endif // SPARK_HAS_IMGUI

} // namespace Terrafront
