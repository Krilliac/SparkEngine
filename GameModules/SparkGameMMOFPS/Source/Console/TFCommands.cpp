/**
 * @file TFCommands.cpp
 * @brief TERRAFRONT console command registration (tf_*).
 *
 * OWNERSHIP: shared registration point — each wave appends its commands here.
 * RegisterConsoleCommands() stays the single entry point; the W2+ surfaces
 * live in the split parts TFCommandsGameplay.cpp / TFCommandsNet.cpp, which
 * it calls in the original registration order. Do not create parallel
 * registrars outside these parts.
 *
 * Surface here (W1 core + W13 tail): tf_status, tf_host, tf_dedicated,
 * tf_connect, tf_disconnect, tf_faction, tf_class, tf_spawn, tf_fire,
 * tf_give, tf_cheat_stats. Handlers stay thin: parse args, call one system,
 * return a string.
 */

#include "Core/SparkGameMMOFPS.h"

#include "Console/TFCommandsInternal.h"
#include "Data/TFDataTables.h"
#include "World/TFWorldSetup.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Net/TFNetProtocol.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFWeaponSystem.h"
#include "Game/TFServerValidation.h" // W13 anti-cheat lane: tf_cheat_stats

#include "Utils/SparkConsole.h"
#include "Spark/IEngineContext.h"
#include "Engine/ECS/Components.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

using namespace Terrafront;
using namespace Terrafront::CommandDetail;

namespace
{

    constexpr uint16_t kDefaultPort = 27020;

    // Client-side class selection shared by tf_class / tf_spawn / tf_give.
    // File-static because TerrafrontModule's header is frozen (no new members).
    ClassId g_selectedClass = ClassId::Striker;

    bool ParseClass(const std::string& arg, ClassId& out)
    {
        const std::string s = Lower(arg);
        if (s == "ghost")
        {
            out = ClassId::Ghost;
            return true;
        }
        if (s == "striker")
        {
            out = ClassId::Striker;
            return true;
        }
        if (s == "medtech")
        {
            out = ClassId::Medtech;
            return true;
        }
        if (s == "fabricator")
        {
            out = ClassId::Fabricator;
            return true;
        }
        if (s == "bulwark")
        {
            out = ClassId::Bulwark;
            return true;
        }
        return false; // Colossus is terminal-purchased, not console-selectable
    }

    const char* ClassName(ClassId c)
    {
        switch (c)
        {
        case ClassId::Ghost:
            return "Ghost";
        case ClassId::Striker:
            return "Striker";
        case ClassId::Medtech:
            return "Medtech";
        case ClassId::Fabricator:
            return "Fabricator";
        case ClassId::Bulwark:
            return "Bulwark";
        case ClassId::Colossus:
            return "Colossus";
        default:
            return "Unknown";
        }
    }

    bool ParsePort(const std::string& arg, uint16_t& out)
    {
        char* end = nullptr;
        const unsigned long v = std::strtoul(arg.c_str(), &end, 10);
        if (end == arg.c_str() || *end != '\0' || v == 0 || v > 65535)
            return false;
        out = static_cast<uint16_t>(v);
        return true;
    }

    // "ip" or "ip:port"
    bool ParseEndpoint(const std::string& arg, std::string& ip, uint16_t& port)
    {
        port = kDefaultPort;
        const size_t colon = arg.rfind(':');
        if (colon == std::string::npos)
        {
            ip = arg;
            return !ip.empty();
        }
        ip = arg.substr(0, colon);
        return !ip.empty() && ParsePort(arg.substr(colon + 1), port);
    }

    const char* RoleName(NetRole r)
    {
        switch (r)
        {
        case NetRole::Standalone:
            return "standalone";
        case NetRole::ListenHost:
            return "listen-host";
        case NetRole::DedicatedServer:
            return "dedicated";
        case NetRole::Client:
            return "client";
        default:
            return "?";
        }
    }

} // namespace

void TerrafrontModule::RegisterConsoleCommands()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const char* cat = "TERRAFRONT";

    // ------------------------------------------------------------------ status
    console.RegisterCommand(
        "tf_status",
        [this](const std::vector<std::string>&) -> std::string
        {
            std::ostringstream os;
            os << "[TF] TERRAFRONT  role=" << RoleName(m_ctx.role) << "  faction=" << FactionTag(m_ctx.localFaction);
            // localFaction is only meaningful where a local player exists; on a
            // dedicated server factions are per-player (this read misled a
            // play-test into "faction=---" panic).
            if (m_ctx.role == NetRole::DedicatedServer)
                os << " (n/a on dedicated server)";
            os << "  player="
               << (m_ctx.localPlayer == kInvalidPlayer ? std::string("-") : std::to_string(m_ctx.localPlayer));

            if (m_ctx.data && m_ctx.data->IsLoaded())
                os << "\n  data: " << m_ctx.data->AllWeapons().size() << " weapons, " << m_ctx.data->AllClasses().size()
                   << " classes, " << m_ctx.data->GetContinent().regions.size() << " regions ("
                   << m_ctx.data->GetContinent().name << ")";
            else
                os << "\n  data: NOT LOADED";

            if (m_ctx.players)
            {
                uint32_t alive = 0;
                m_ctx.players->ForEachAlivePawn([&](const auto&) { ++alive; });
                os << "\n  pawns alive: " << alive;
            }

            os << "\n  client: " << (ClientConnected(m_ctx) ? "connected" : "not connected");
            if (m_ctx.role != NetRole::DedicatedServer)
                os << "\n  session: " << (m_ctx.InWorld() ? "in-world" : "onboarding");
            if (m_ctx.IsAuthority() && m_ctx.serverSim && m_ctx.role != NetRole::Standalone)
            {
                char t[32];
                std::snprintf(t, sizeof(t), "%.1f", m_ctx.serverSim->ServerTime());
                os << "\n  server time: " << t << "s";
            }
#ifdef ENABLE_NETWORKING
            {
                auto& nm = Spark::Net::NetworkManager::GetInstance();
                if (nm.IsInitialized())
                {
                    const auto& st = nm.GetStats();
                    char net[128];
                    std::snprintf(net, sizeof(net), "\n  net: ping %.0fms  loss %.1f%%  up %.1fKB/s  down %.1fKB/s",
                                  st.ping, st.packetLoss * 100.0f, st.bandwidthUp, st.bandwidthDown);
                    os << net;
                }
            }
#endif
            return os.str();
        },
        "Show TERRAFRONT module status", cat, "tf_status");

    // ------------------------------------------------------------- net booting
    console.RegisterCommand(
        "tf_host",
        [this](const std::vector<std::string>& args) -> std::string
        {
            uint16_t port = kDefaultPort;
            if (!args.empty() && !ParsePort(args[0], port))
                return "[TF] tf_host: bad port '" + args[0] + "'";
            if (!m_ctx.world)
                return "[TF] world system not ready";
            return m_ctx.world->StartHost(port) ? "[TF] listen host started on port " + std::to_string(port)
                                                : "[TF] failed to start listen host (see log)";
        },
        "Start a listen host (in-process server + local player)", cat, "tf_host [port=27020]");

    console.RegisterCommand(
        "tf_dedicated",
        [this](const std::vector<std::string>& args) -> std::string
        {
            uint16_t port = kDefaultPort;
            if (!args.empty() && !ParsePort(args[0], port))
                return "[TF] tf_dedicated: bad port '" + args[0] + "'";
            if (!m_ctx.world)
                return "[TF] world system not ready";
            return m_ctx.world->StartDedicated(port) ? "[TF] dedicated server started on port " + std::to_string(port)
                                                     : "[TF] failed to start dedicated server (see log)";
        },
        "Start a dedicated (headless) server", cat, "tf_dedicated [port=27020]");

    console.RegisterCommand(
        "tf_connect",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "[TF] usage: tf_connect <ip[:port]>";
            std::string ip;
            uint16_t port = kDefaultPort;
            if (!ParseEndpoint(args[0], ip, port))
                return "[TF] tf_connect: bad endpoint '" + args[0] + "'";
            if (!m_ctx.world)
                return "[TF] world system not ready";
            return m_ctx.world->Connect(ip, port) ? "[TF] connecting to " + ip + ":" + std::to_string(port)
                                                  : "[TF] connect failed (see log)";
        },
        "Connect to a TERRAFRONT server", cat, "tf_connect <ip[:port]>");

    console.RegisterCommand(
        "tf_disconnect",
        [this](const std::vector<std::string>&) -> std::string
        {
#ifdef ENABLE_NETWORKING
            // TF-W2: route through a TFWorldSetup stop/teardown API so world
            // state (role, servers, scene) resets cleanly alongside the socket.
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (!nm.IsInitialized())
                return "[TF] networking not initialized";
            if (m_ctx.role == NetRole::Client)
                nm.Disconnect();
            else
                nm.StopServer();
            return "[TF] disconnected";
#else
            return "[TF] networking disabled in this build (define ENABLE_NETWORKING)";
#endif
        },
        "Disconnect from server / stop hosting", cat, "tf_disconnect");

    // ----------------------------------------------------------- player intent
    console.RegisterCommand(
        "tf_faction",
        [this](const std::vector<std::string>& args) -> std::string
        {
            FactionId f;
            if (args.empty() || !ParseFaction(args[0], f))
                return "[TF] usage: tf_faction <mra|auc|hlx>";
            m_ctx.localFaction = f; // immediate local feedback (HUD tinting)
            if (ClientConnected(m_ctx))
            {
                TF_FactionSelect msg{};
                msg.faction = static_cast<uint8_t>(f);
                m_ctx.clientNet->SendMsg(TFMsg::FactionSelect, &msg, sizeof(msg));
            }
            return std::string("[TF] faction set: ") + FactionName(f) +
                   (ClientConnected(m_ctx) ? " (sent to server)" : " (local only - not connected)");
        },
        "Choose your faction", cat, "tf_faction <mra|auc|hlx>");

    console.RegisterCommand(
        "tf_class",
        [this](const std::vector<std::string>& args) -> std::string
        {
            ClassId c;
            if (args.empty() || !ParseClass(args[0], c))
                return "[TF] usage: tf_class <ghost|striker|medtech|fabricator|bulwark>";
            g_selectedClass = c;
            if (ClientConnected(m_ctx))
            {
                // kInvalidWeapon in every slot == "class defaults" (server resolves).
                TF_LoadoutChange msg{};
                msg.classId = static_cast<uint8_t>(c);
                msg.primary = kInvalidWeapon;
                msg.secondary = kInvalidWeapon;
                msg.tool = kInvalidWeapon;
                m_ctx.clientNet->SendMsg(TFMsg::LoadoutChange, &msg, sizeof(msg));
            }
            return std::string("[TF] class selected: ") + ClassName(c) + " (applies on next deploy)";
        },
        "Select the class used on next spawn", cat, "tf_class <ghost|striker|medtech|fabricator|bulwark>");

    console.RegisterCommand(
        "tf_spawn",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (m_ctx.localFaction == FactionId::None)
                return "[TF] pick a faction first: tf_faction <mra|auc|hlx>";
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - use tf_host or tf_connect first";
            TF_SpawnRequest rq{};
            rq.classId = static_cast<uint8_t>(g_selectedClass);
            rq.spawnKind = 0; // skyanchor
            m_ctx.clientNet->SendMsg(TFMsg::SpawnRequest, &rq, sizeof(rq));
            return std::string("[TF] deploy requested: ") + ClassName(g_selectedClass) + " @ skyanchor";
        },
        "Deploy at your faction skyanchor", cat, "tf_spawn");

    console.RegisterCommand(
        "tf_fire",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_ctx.weapons)
                return "[TF] weapon system not ready";
            m_ctx.weapons->ClientTriggerFire();
            return "[TF] fired";
        },
        "Fire the active weapon once (debug/smoke)", cat, "tf_fire");

    console.RegisterCommand(
        "tf_give",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
                return "[TF] usage: tf_give <weaponKey>";
            if (!m_ctx.data || !m_ctx.data->IsLoaded())
                return "[TF] data tables not loaded";
            const WeaponDef* def = m_ctx.data->GetWeaponByKey(args[0]);
            if (!def)
                return "[TF] unknown weapon key '" + args[0] + "' (see weapons.json)";
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - weapon grants go through the server";
            TF_LoadoutChange msg{};
            msg.classId = static_cast<uint8_t>(g_selectedClass);
            msg.primary = def->id;
            msg.secondary = kInvalidWeapon;
            msg.tool = kInvalidWeapon;
            m_ctx.clientNet->SendMsg(TFMsg::LoadoutChange, &msg, sizeof(msg));
            return "[TF] requested primary: " + def->name + " (" + def->key + ")";
        },
        "Request a weapon as your primary", cat, "tf_give <weaponKey>");

    // Split parts, called in the original registration order: data/world +
    // debug/move + W2/W3 gameplay first, then chat + W5 onboarding.
    RegisterConsoleCommandsGameplay(); // Console/TFCommandsGameplay.cpp
    RegisterConsoleCommandsNet();      // Console/TFCommandsNet.cpp

    // ------------------------------------------------------------------- W13
    // Anti-cheat lane: per-player detection/clamp/reject counters (movement
    // speed-hack/teleport clamps, TFWeaponSystem::ValidateFire RoF rejects,
    // TF_FireEvent claimed-origin rejects, TFServerSim::EnqueueInput input-rate
    // token-bucket rejects). Everything this wave is detection + clamp/reject
    // only — no bans — so this command is the only visibility into who's
    // been tripping the checks (see Game/TFServerValidation.h for the full
    // design writeup).
    console.RegisterCommand(
        "tf_cheat_stats",
        [this](const std::vector<std::string>&) -> std::string
        {
            const auto& stats = TFServerValidation::Get().Stats();
            if (stats.empty())
            {
                return m_ctx.IsAuthority() ? "[TF] anti-cheat: no violations recorded"
                                           : "[TF] anti-cheat: no data (this instance is not the server)";
            }
            std::ostringstream os;
            os << "[TF] anti-cheat violations (" << stats.size() << " player" << (stats.size() == 1 ? "" : "s") << "):";
            for (const auto& [player, st] : stats)
            {
                os << "\n  p" << player << "  moveClamps=" << st.movementClamps << " (spikes=" << st.movementSpikes
                   << ")  fireRateRejects=" << st.fireRateRejects << "  fireOriginRejects=" << st.fireOriginRejects
                   << "  inputRateRejects=" << st.inputRateRejects;
            }
            return os.str();
        },
        "Anti-cheat: dump per-player violation counters (authority only)", cat, "tf_cheat_stats");
}
