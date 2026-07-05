/**
 * @file TFCommands.cpp
 * @brief TERRAFRONT console command registration (tf_*).
 *
 * OWNERSHIP: shared registration point — each wave appends its commands here.
 * Keep one registration function; do not create parallel registrars.
 *
 * W1 surface: tf_status, tf_host, tf_dedicated, tf_connect, tf_disconnect,
 * tf_faction, tf_spawn, tf_class, tf_give, tf_reload_data, tf_regions,
 * tf_pos, tf_tp. Handlers stay thin: parse args, call one system, return a
 * string. W2+ adds: tf_flux, tf_capture, tf_vehicle, tf_bots.
 */

#include "Core/SparkGameMMOFPS.h"

#include "Data/TFDataTables.h"
#include "World/TFWorldSetup.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Net/TFNetProtocol.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFBotSystem.h"
#include "Game/TFWeaponSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFColossusSystem.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFProgressionSystem.h"
#include "World/TFRegionSystem.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"

#include "Camera/SparkEngineCamera.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"
#include "Spark/IEngineContext.h"
#include "Engine/ECS/Components.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h"
#endif

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>

using namespace Terrafront;

namespace {

constexpr uint16_t kDefaultPort = 27020;

// Client-side class selection shared by tf_class / tf_spawn / tf_give.
// File-static because TerrafrontModule's header is frozen (no new members).
ClassId g_selectedClass = ClassId::Striker;

std::string Lower(std::string s)
{
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ParseFaction(const std::string& arg, FactionId& out)
{
    const std::string s = Lower(arg);
    if (s == "mra") { out = FactionId::MRA; return true; }
    if (s == "auc") { out = FactionId::AUC; return true; }
    if (s == "hlx") { out = FactionId::HLX; return true; }
    return false;
}

bool ParseClass(const std::string& arg, ClassId& out)
{
    const std::string s = Lower(arg);
    if (s == "ghost")      { out = ClassId::Ghost;      return true; }
    if (s == "striker")    { out = ClassId::Striker;    return true; }
    if (s == "medtech")    { out = ClassId::Medtech;    return true; }
    if (s == "fabricator") { out = ClassId::Fabricator; return true; }
    if (s == "bulwark")    { out = ClassId::Bulwark;    return true; }
    return false;   // Colossus is terminal-purchased, not console-selectable
}

const char* ClassName(ClassId c)
{
    switch (c) {
        case ClassId::Ghost:      return "Ghost";
        case ClassId::Striker:    return "Striker";
        case ClassId::Medtech:    return "Medtech";
        case ClassId::Fabricator: return "Fabricator";
        case ClassId::Bulwark:    return "Bulwark";
        case ClassId::Colossus:   return "Colossus";
        default:                  return "Unknown";
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
    if (colon == std::string::npos) {
        ip = arg;
        return !ip.empty();
    }
    ip = arg.substr(0, colon);
    return !ip.empty() && ParsePort(arg.substr(colon + 1), port);
}

bool ParseFloat(const std::string& arg, float& out)
{
    char* end = nullptr;
    out = std::strtof(arg.c_str(), &end);
    return end != arg.c_str() && *end == '\0';
}

const char* RoleName(NetRole r)
{
    switch (r) {
        case NetRole::Standalone:      return "standalone";
        case NetRole::ListenHost:      return "listen-host";
        case NetRole::DedicatedServer: return "dedicated";
        case NetRole::Client:          return "client";
        default:                       return "?";
    }
}

bool ClientConnected(const TFGameContext& ctx)
{
    return ctx.clientNet != nullptr && ctx.clientNet->IsConnected();
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
            os << "[TF] TERRAFRONT  role=" << RoleName(m_ctx.role)
               << "  faction=" << FactionTag(m_ctx.localFaction)
               << "  player=" << (m_ctx.localPlayer == kInvalidPlayer
                                      ? std::string("-")
                                      : std::to_string(m_ctx.localPlayer));

            if (m_ctx.data && m_ctx.data->IsLoaded())
                os << "\n  data: " << m_ctx.data->AllWeapons().size() << " weapons, "
                   << m_ctx.data->AllClasses().size() << " classes, "
                   << m_ctx.data->GetContinent().regions.size() << " regions ("
                   << m_ctx.data->GetContinent().name << ")";
            else
                os << "\n  data: NOT LOADED";

            if (m_ctx.players) {
                uint32_t alive = 0;
                m_ctx.players->ForEachAlivePawn([&](const auto&) { ++alive; });
                os << "\n  pawns alive: " << alive;
            }

            os << "\n  client: " << (ClientConnected(m_ctx) ? "connected" : "not connected");
            if (m_ctx.IsAuthority() && m_ctx.serverSim && m_ctx.role != NetRole::Standalone) {
                char t[32];
                std::snprintf(t, sizeof(t), "%.1f", m_ctx.serverSim->ServerTime());
                os << "\n  server time: " << t << "s";
            }
#ifdef ENABLE_NETWORKING
            {
                auto& nm = Spark::Net::NetworkManager::GetInstance();
                if (nm.IsInitialized()) {
                    const auto& st = nm.GetStats();
                    char net[128];
                    std::snprintf(net, sizeof(net),
                                  "\n  net: ping %.0fms  loss %.1f%%  up %.1fKB/s  down %.1fKB/s",
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
            return m_ctx.world->StartHost(port)
                       ? "[TF] listen host started on port " + std::to_string(port)
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
            return m_ctx.world->StartDedicated(port)
                       ? "[TF] dedicated server started on port " + std::to_string(port)
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
            return m_ctx.world->Connect(ip, port)
                       ? "[TF] connecting to " + ip + ":" + std::to_string(port)
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
            m_ctx.localFaction = f;   // immediate local feedback (HUD tinting)
            if (ClientConnected(m_ctx)) {
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
            if (ClientConnected(m_ctx)) {
                // kInvalidWeapon in every slot == "class defaults" (server resolves).
                TF_LoadoutChange msg{};
                msg.classId   = static_cast<uint8_t>(c);
                msg.primary   = kInvalidWeapon;
                msg.secondary = kInvalidWeapon;
                msg.tool      = kInvalidWeapon;
                m_ctx.clientNet->SendMsg(TFMsg::LoadoutChange, &msg, sizeof(msg));
            }
            return std::string("[TF] class selected: ") + ClassName(c) +
                   " (applies on next deploy)";
        },
        "Select the class used on next spawn", cat,
        "tf_class <ghost|striker|medtech|fabricator|bulwark>");

    console.RegisterCommand(
        "tf_spawn",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (m_ctx.localFaction == FactionId::None)
                return "[TF] pick a faction first: tf_faction <mra|auc|hlx>";
            if (!ClientConnected(m_ctx))
                return "[TF] not connected - use tf_host or tf_connect first";
            TF_SpawnRequest rq{};
            rq.classId   = static_cast<uint8_t>(g_selectedClass);
            rq.spawnKind = 0;   // skyanchor
            m_ctx.clientNet->SendMsg(TFMsg::SpawnRequest, &rq, sizeof(rq));
            return std::string("[TF] deploy requested: ") + ClassName(g_selectedClass) +
                   " @ skyanchor";
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
            msg.classId   = static_cast<uint8_t>(g_selectedClass);
            msg.primary   = def->id;
            msg.secondary = kInvalidWeapon;
            msg.tool      = kInvalidWeapon;
            m_ctx.clientNet->SendMsg(TFMsg::LoadoutChange, &msg, sizeof(msg));
            return "[TF] requested primary: " + def->name + " (" + def->key + ")";
        },
        "Request a weapon as your primary", cat, "tf_give <weaponKey>");

    // -------------------------------------------------------------- data/world
    console.RegisterCommand(
        "tf_reload_data",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_ctx.data)
                return "[TF] data system not ready";
            return m_ctx.data->ReloadAll()
                       ? "[TF] data tables reloaded"
                       : "[TF] data reload FAILED - previous tables kept (see log)";
        },
        "Hot-reload all JSON data tables", cat, "tf_reload_data");

    console.RegisterCommand(
        "tf_regions",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_ctx.data || !m_ctx.data->IsLoaded())
                return "[TF] data tables not loaded";
            if (!m_ctx.regions)
                return "[TF] region system not ready";
            const ContinentDef& cont = m_ctx.data->GetContinent();
            std::ostringstream os;
            os << "[TF] " << cont.name << " - live territory:";
            for (const RegionDef& r : cont.regions)
            {
                FactionId capturing = FactionId::None;
                bool contested = false;
                const float prog =
                    m_ctx.regions->CaptureProgress(r.id, capturing, contested);
                os << "\n  [" << static_cast<int>(r.id) << "] " << r.name << " (" << r.tier
                   << ") owner=" << FactionTag(m_ctx.regions->OwnerOf(r.id));
                if (prog > 0.0f)
                    os << "  cap " << static_cast<int>(prog * 100.0f) << "% by "
                       << FactionTag(capturing) << (contested ? " CONTESTED" : "");
            }
            os << "\n  held: MRA " << m_ctx.regions->RegionsHeld(FactionId::MRA)
               << " / AUC " << m_ctx.regions->RegionsHeld(FactionId::AUC)
               << " / HLX " << m_ctx.regions->RegionsHeld(FactionId::HLX);
            FactionId domF = FactionId::None; float domLeft = 0.0f;
            if (m_ctx.regions->DominionActive(domF, domLeft))
                os << "\n  DOMINION: " << FactionName(domF) << " ("
                   << static_cast<int>(domLeft) << "s)";
            return os.str();
        },
        "List regions and ownership", cat, "tf_regions");

    // -------------------------------------------------------------- debug/move
    console.RegisterCommand(
        "tf_pos",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_ctx.players || m_ctx.localPlayer == kInvalidPlayer)
                return "[TF] no local player";
            PawnInfo p{};
            if (!m_ctx.players->GetPawnByPlayer(m_ctx.localPlayer, p))
                return "[TF] no pawn (deploy with tf_spawn)";
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "[TF] pos (%.1f, %.1f, %.1f)  yaw %.2f  %s",
                          p.pos[0], p.pos[1], p.pos[2], p.yaw,
                          p.alive ? "alive" : "dead");
            return std::string(buf);
        },
        "Print local pawn position", cat, "tf_pos");

    console.RegisterCommand(
        "tf_tp",
        [this](const std::vector<std::string>& args) -> std::string
        {
            float x, z;
            if (args.size() < 2 || !ParseFloat(args[0], x) || !ParseFloat(args[1], z))
                return "[TF] usage: tf_tp <x> <z>";
            if (!m_ctx.IsAuthority())
                return "[TF] tf_tp is server-side only (TF-W2: admin command routing)";
            if (!m_ctx.players || !m_ctx.world || m_ctx.localPlayer == kInvalidPlayer)
                return "[TF] no local player pawn to teleport";
            PawnInfo p{};
            if (!m_ctx.players->GetPawnByPlayer(m_ctx.localPlayer, p) || !p.alive)
                return "[TF] no living pawn (deploy with tf_spawn)";

            if (!m_ctx.serverSim)
                return "[TF] server sim unavailable";
            const float y = m_ctx.world->TerrainHeightAt(x, z) + 1.5f;
            // Through the authoritative MoveState — writing the Transform
            // directly was silently undone by the next movement tick.
            m_ctx.serverSim->TeleportPawn(m_ctx.localPlayer, x, y, z);
            char buf[96];
            std::snprintf(buf, sizeof(buf), "[TF] teleported to (%.1f, %.1f, %.1f)", x, y, z);
            return std::string(buf);
        },
        "Teleport your pawn (authority only)", cat, "tf_tp <x> <z>");

    // ------------------------------------------------------------------- W2
    console.RegisterCommand(
        "tf_bots",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_bots)
                return "[TF] bot system not ready";
            if (args.empty())
                return "[TF] bots active: " + std::to_string(m_bots->BotCount()) +
                       "  (tf_bots <0-32> to set)";
            const int n = std::atoi(args[0].c_str());
            if (n < 0 || n > 32)
                return "[TF] tf_bots: count must be 0-32";
            m_bots->ServerSetBotCount(static_cast<uint32_t>(n));
            return "[TF] bot count set to " + std::to_string(n);
        },
        "Spawn server-side bots (authority only)", cat, "tf_bots <n>");

    console.RegisterCommand(
        "tf_capture",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_ctx.regions)
                return "[TF] region system not ready";
            if (args.size() < 2)
                return "Usage: tf_capture <regionId> <mra|auc|hlx|none>";
            const RegionId id = static_cast<RegionId>(std::atoi(args[0].c_str()));
            FactionId f = FactionId::None;
            if (Lower(args[1]) != "none" && !ParseFaction(args[1], f))
                return "[TF] bad faction '" + args[1] + "'";
            return m_ctx.regions->ServerForceOwner(id, f)
                       ? "[TF] region " + args[0] + " -> " + FactionTag(f)
                       : "[TF] capture refused (authority only / bad id / skyanchor)";
        },
        "Debug: force region ownership (authority only)", cat,
        "tf_capture <regionId> <mra|auc|hlx|none>");

    console.RegisterCommand(
        "tf_botinfo",
        [this](const std::vector<std::string>&) -> std::string
        {
            return m_bots ? m_bots->DebugSummary() : "[TF] bot system not ready";
        },
        "Per-bot diagnostic dump (state, pawn, position, scheduling)", cat, "tf_botinfo");

    console.RegisterCommand(
        "tf_cam",
        [this](const std::vector<std::string>& args) -> std::string
        {
            // Module-owned camera (engine cam_* commands can't see it: the
            // exe-side EngineContext registry never gets a camera in module
            // mode). Used by the automated smoke to frame screenshots.
            SparkEngineCamera* cam = m_ctx.world ? m_ctx.world->GetCamera() : nullptr;
            if (!cam)
                return "[TF] no camera (headless?)";
            if (args.size() < 2)
                return "[TF] usage: tf_cam <pitchDeg> <yawDeg> [x y z]";
            try
            {
                cam->Console_SetRotation(std::stof(args[0]), std::stof(args[1]), 0.0f);
                if (args.size() >= 5)
                    cam->SetPosition({std::stof(args[2]), std::stof(args[3]), std::stof(args[4])});
            }
            catch (const std::exception&)
            {
                return "[TF] tf_cam: bad number";
            }
            // Echo the resulting pose — the smoke log uses this to verify the
            // first-person camera is where the script thinks it is.
            const auto cp = cam->GetPosition();
            const auto cf = cam->GetForward();
            const auto cr = cam->GetRotation();
            char info[160];
            std::snprintf(info, sizeof(info),
                          "[TF] camera set: pos(%.1f %.1f %.1f) pitch %.1f yaw %.1f fwd(%.2f %.2f %.2f)",
                          cp.x, cp.y, cp.z, cr.x * 57.2958f, cr.y * 57.2958f, cf.x, cf.y, cf.z);
            return std::string(info);
        },
        "Set camera rotation (deg) and optional position", cat,
        "tf_cam <pitchDeg> <yawDeg> [x y z]");

    console.RegisterCommand(
        "tf_map",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_map)
                return "[TF] map not ready";
            m_map->Toggle();
            return m_map->IsOpen() ? "[TF] map opened" : "[TF] map closed";
        },
        "Toggle the continent map", cat, "tf_map");

    console.RegisterCommand(
        "tf_deploy",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_spawnUI)
                return "[TF] spawn screen not ready";
            m_spawnUI->Open();
            return "[TF] deploy screen opened";
        },
        "Open the deploy/spawn screen", cat, "tf_deploy");

    console.RegisterCommand(
        "tf_flux",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_ctx.progression)
                return "[TF] progression not ready";
            const PlayerId me = m_ctx.localPlayer;
            if (me == kInvalidPlayer)
                return "[TF] no local player yet (tf_spawn first)";
            std::ostringstream os;
            os << "[TF] rank " << m_ctx.progression->RankOf(me)
               << "  xp " << m_ctx.progression->XPOf(me)
               << "  flux " << m_ctx.progression->FluxOf(me);
            return os.str();
        },
        "Show your rank/XP/flux", cat, "tf_flux");

    console.RegisterCommand(
        "tf_save",
        [this](const std::vector<std::string>&) -> std::string
        {
            bool terr = m_ctx.regions && m_ctx.regions->SaveNow();
            bool prog = m_ctx.progression && m_ctx.progression->SaveNow();
            std::ostringstream os;
            os << "[TF] save: territory " << (terr ? "ok" : "skipped")
               << ", progression " << (prog ? "ok" : "skipped");
            return os.str();
        },
        "Force-save territory + progression (authority only)", cat, "tf_save");

    console.RegisterCommand(
        "tf_debug",
        [this](const std::vector<std::string>& args) -> std::string
        {
            const std::string what = args.empty() ? "" : Lower(args[0]);
            if (what == "server" && m_serverSim)   { m_serverSim->ToggleDebugUI();   return "[TF] server debug toggled"; }
            if (what == "regions" && m_ctx.regions){ m_ctx.regions->ToggleDebugUI(); return "[TF] regions debug toggled"; }
            if (what == "bots" && m_bots)          { m_bots->ToggleDebugUI();        return "[TF] bots debug toggled"; }
            if (what == "net" && m_clientNet)      { m_clientNet->ToggleDebugUI();   return "[TF] net debug toggled"; }
            if (what == "progression" && m_ctx.progression) { m_ctx.progression->ToggleDebugUI(); return "[TF] progression debug toggled"; }
            if (what == "panels" || what == "all")
            {
                m_debugPanels = !m_debugPanels;
                return m_debugPanels ? "[TF] debug panels shown" : "[TF] debug panels hidden";
            }
            return "Usage: tf_debug <panels|server|regions|bots|net|progression>";
        },
        "Toggle a TF debug panel", cat, "tf_debug <system>");

    // ------------------------------------------------------------------- W3
    console.RegisterCommand(
        "tf_vehicle",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_ctx.vehicles)
                return "[TF] vehicle system not ready";
            if (args.empty())
                return "Usage: tf_vehicle <drifter|aegis|ravager>";
            const std::string v = Lower(args[0]);
            VehicleId id = VehicleId::None;
            if (v == "drifter") id = VehicleId::Drifter;
            else if (v == "aegis") id = VehicleId::Aegis;
            else if (v == "ravager") id = VehicleId::Ravager;
            else if (v == "vulture") id = VehicleId::Vulture;
            else return "[TF] unknown vehicle '" + args[0] + "'";
            return m_ctx.vehicles->ServerPurchaseVehicle(m_ctx.localPlayer, id)
                       ? "[TF] vehicle purchased - check the terminal pad"
                       : "[TF] purchase refused (flux / terminal range / region ownership)";
        },
        "Buy a vehicle at a friendly terminal", cat, "tf_vehicle <kind>");

    console.RegisterCommand(
        "tf_colossus",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_ctx.colossus)
                return "[TF] colossus system not ready";
            const TFColossusResult r = m_ctx.colossus->ServerPurchaseColossus(m_ctx.localPlayer);
            return r == TFColossusResult::Ok
                       ? std::string("[TF] Colossus suit engaged")
                       : std::string("[TF] refused: ") + TFColossusSystem::ResultText(r);
        },
        "Purchase a Colossus exosuit at a terminal", cat, "tf_colossus");

    console.RegisterCommand(
        "tf_place",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_ctx.deployables)
                return "[TF] deployable system not ready";
            if (args.empty())
                return "Usage: tf_place <turret|ammo|beacon>";
            const std::string d = Lower(args[0]);
            DeployableKind kind;
            if (d == "turret") kind = DeployableKind::FabTurret;
            else if (d == "ammo") kind = DeployableKind::FabAmmoPack;
            else if (d == "beacon") kind = DeployableKind::MedBeacon;
            else return "[TF] unknown deployable '" + args[0] + "'";
            const TFDeployResult r =
                m_ctx.deployables->ServerTryPlaceDeployable(m_ctx.localPlayer, kind);
            return r == TFDeployResult::Ok
                       ? std::string("[TF] deployable placed")
                       : std::string("[TF] refused: ") + TFDeployableSystem::ResultText(r);
        },
        "Place a deployable (Fabricator/Medtech)", cat, "tf_place <kind>");

    console.RegisterCommand(
        "tf_giveflux",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_ctx.progression)
                return "[TF] progression not ready";
            const uint32_t n = args.empty() ? 750u
                : static_cast<uint32_t>(std::max(0, std::atoi(args[0].c_str())));
            m_ctx.progression->ServerGrantFlux(m_ctx.localPlayer, n);
            return "[TF] granted " + std::to_string(n) + " flux (debug)";
        },
        "Debug: grant yourself flux", cat, "tf_giveflux [n=750]");
}
