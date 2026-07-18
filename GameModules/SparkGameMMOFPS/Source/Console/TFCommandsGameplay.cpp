/**
 * @file TFCommandsGameplay.cpp
 * @brief TERRAFRONT console commands — data/world, debug/move and W2/W3
 *        gameplay surface (split part of TFCommands.cpp).
 *
 * Registered from TerrafrontModule::RegisterConsoleCommands() so the single
 * registration entry point (and its order) is preserved. Surface here:
 * tf_reload_data, tf_regions, tf_pos, tf_tp, tf_bots, tf_capture, tf_botinfo,
 * tf_cam, tf_map, tf_deploy, tf_flux, tf_save, tf_debug, tf_perf, tf_vehicle,
 * tf_colossus, tf_place, tf_giveflux, tf_unlock.
 */

#include "Core/SparkGameMMOFPS.h"

#include "Console/TFCommandsInternal.h"
#include "Data/TFDataTables.h"
#include "World/TFWorldSetup.h"
#include "Net/TFClientNet.h"
#include "Net/TFServerSim.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFBotSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFColossusSystem.h"
#include "Game/TFDeployableSystem.h"
#include "Game/TFDeployableTypes.h" // W6 deployables: extended kind catalog
#include "Game/TFProgressionSystem.h"
#include "Persistence/TFUnlockTree.h" // W6 progression: tf_unlock
#include "World/TFRegionSystem.h"
#include "UI/TFMapScreen.h"
#include "UI/TFSpawnScreen.h"

#include "Camera/SparkEngineCamera.h"
#include "Utils/SparkConsole.h"
#include "Utils/TFPerfCounters.h" // TF-W13 server-perf lane: tf_perf

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <sstream>
#include <string>
#include <vector>

using namespace Terrafront;
using namespace Terrafront::CommandDetail;

namespace
{

    bool ParseFloat(const std::string& arg, float& out)
    {
        char* end = nullptr;
        out = std::strtof(arg.c_str(), &end);
        return end != arg.c_str() && *end == '\0';
    }

} // namespace

void TerrafrontModule::RegisterConsoleCommandsGameplay()
{
    auto& console = Spark::SimpleConsole::GetInstance();
    const char* cat = "TERRAFRONT";

    // -------------------------------------------------------------- data/world
    console.RegisterCommand(
        "tf_reload_data",
        [this](const std::vector<std::string>&) -> std::string
        {
            if (!m_ctx.data)
                return "[TF] data system not ready";
            return m_ctx.data->ReloadAll() ? "[TF] data tables reloaded"
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
                const float prog = m_ctx.regions->CaptureProgress(r.id, capturing, contested);
                os << "\n  [" << static_cast<int>(r.id) << "] " << r.name << " (" << r.tier
                   << ") owner=" << FactionTag(m_ctx.regions->OwnerOf(r.id));
                if (prog > 0.0f)
                    os << "  cap " << static_cast<int>(prog * 100.0f) << "% by " << FactionTag(capturing)
                       << (contested ? " CONTESTED" : "");
            }
            os << "\n  held: MRA " << m_ctx.regions->RegionsHeld(FactionId::MRA) << " / AUC "
               << m_ctx.regions->RegionsHeld(FactionId::AUC) << " / HLX " << m_ctx.regions->RegionsHeld(FactionId::HLX);
            FactionId domF = FactionId::None;
            float domLeft = 0.0f;
            if (m_ctx.regions->DominionActive(domF, domLeft))
                os << "\n  DOMINION: " << FactionName(domF) << " (" << static_cast<int>(domLeft) << "s)";
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
            std::snprintf(buf, sizeof(buf), "[TF] pos (%.1f, %.1f, %.1f)  yaw %.2f  %s", p.pos[0], p.pos[1], p.pos[2],
                          p.yaw, p.alive ? "alive" : "dead");
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
                return "[TF] bots active: " + std::to_string(m_bots->BotCount()) + "  (tf_bots <0-32> to set)";
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
        "Debug: force region ownership (authority only)", cat, "tf_capture <regionId> <mra|auc|hlx|none>");

    console.RegisterCommand(
        "tf_botinfo", [this](const std::vector<std::string>&) -> std::string
        { return m_bots ? m_bots->DebugSummary() : "[TF] bot system not ready"; },
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
                          "[TF] camera set: pos(%.1f %.1f %.1f) pitch %.1f yaw %.1f fwd(%.2f %.2f %.2f)", cp.x, cp.y,
                          cp.z, cr.x * 57.2958f, cr.y * 57.2958f, cf.x, cf.y, cf.z);
            return std::string(info);
        },
        "Set camera rotation (deg) and optional position", cat, "tf_cam <pitchDeg> <yawDeg> [x y z]");

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
            os << "[TF] rank " << m_ctx.progression->RankOf(me) << "  xp " << m_ctx.progression->XPOf(me) << "  flux "
               << m_ctx.progression->FluxOf(me);
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
            os << "[TF] save: territory " << (terr ? "ok" : "skipped") << ", progression " << (prog ? "ok" : "skipped");
            return os.str();
        },
        "Force-save territory + progression (authority only)", cat, "tf_save");

    console.RegisterCommand(
        "tf_debug",
        [this](const std::vector<std::string>& args) -> std::string
        {
            const std::string what = args.empty() ? "" : Lower(args[0]);
            if (what == "server" && m_serverSim)
            {
                m_serverSim->ToggleDebugUI();
                return "[TF] server debug toggled";
            }
            if (what == "regions" && m_ctx.regions)
            {
                m_ctx.regions->ToggleDebugUI();
                return "[TF] regions debug toggled";
            }
            if (what == "bots" && m_bots)
            {
                m_bots->ToggleDebugUI();
                return "[TF] bots debug toggled";
            }
            if (what == "net" && m_clientNet)
            {
                m_clientNet->ToggleDebugUI();
                return "[TF] net debug toggled";
            }
            if (what == "progression" && m_ctx.progression)
            {
                m_ctx.progression->ToggleDebugUI();
                return "[TF] progression debug toggled";
            }
            if (what == "panels" || what == "all")
            {
                m_debugPanels = !m_debugPanels;
                return m_debugPanels ? "[TF] debug panels shown" : "[TF] debug panels hidden";
            }
            return "Usage: tf_debug <panels|server|regions|bots|net|progression>";
        },
        "Toggle a TF debug panel", cat, "tf_debug <system>");

    // TF-W13 server-perf lane: per-phase tick averages/peaks + tick budget
    // headroom. Works standalone/listen-host too (the authoritative tick
    // runs there regardless of ENABLE_NETWORKING), so this is deliberately
    // NOT inside the ENABLE_NETWORKING block (see TFCommandsNet.cpp).
    // `tf_perf reset` clears the ring buffers (useful for isolating one
    // chaos-harness run).
    console.RegisterCommand(
        "tf_perf",
        [](const std::vector<std::string>& args) -> std::string
        {
            auto& perf = Terrafront::TFPerfCounters::Instance();
            if (!args.empty() && Lower(args[0]) == "reset")
            {
                perf.Reset();
                return "[TF] perf counters reset";
            }
            return perf.Report(Terrafront::kServerTickHz);
        },
        "Server tick phase timing (avg/peak ms) + budget headroom", cat, "tf_perf [reset]");

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
            if (v == "drifter")
                id = VehicleId::Drifter;
            else if (v == "aegis")
                id = VehicleId::Aegis;
            else if (v == "ravager")
                id = VehicleId::Ravager;
            else if (v == "vulture")
                id = VehicleId::Vulture;
            else
                return "[TF] unknown vehicle '" + args[0] + "'";
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
            return r == TFColossusResult::Ok ? std::string("[TF] Colossus suit engaged")
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
                return "Usage: tf_place <turret|ammo|beacon|resupply|avturret|wall>";
            const std::string d = Lower(args[0]);
            DeployableKind kind;
            if (d == "turret")
                kind = DeployableKind::FabTurret;
            else if (d == "ammo")
                kind = DeployableKind::FabAmmoPack;
            else if (d == "beacon")
                kind = DeployableKind::MedBeacon;
            else if (d == "resupply")
                kind = kDeployResupplyStation;
            else if (d == "avturret")
                kind = kDeployAVTurret;
            else if (d == "wall")
                kind = kDeployShieldWall;
            else
                return "[TF] unknown deployable '" + args[0] + "'";
            const TFDeployResult r = m_ctx.deployables->ServerTryPlaceDeployable(m_ctx.localPlayer, kind);
            return r == TFDeployResult::Ok ? std::string("[TF] deployable placed")
                                           : std::string("[TF] refused: ") + TFDeployableSystem::ResultText(r);
        },
        "Place a deployable (Fabricator/Medtech)", cat, "tf_place <kind>");

    console.RegisterCommand(
        "tf_giveflux",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_ctx.progression)
                return "[TF] progression not ready";
            const uint32_t n = args.empty() ? 750u : static_cast<uint32_t>(std::max(0, std::atoi(args[0].c_str())));
            m_ctx.progression->ServerGrantFlux(m_ctx.localPlayer, n);
            return "[TF] granted " + std::to_string(n) + " flux (debug)";
        },
        "Debug: grant yourself flux", cat, "tf_giveflux [n=750]");

    console.RegisterCommand(
        "tf_unlock",
        [this](const std::vector<std::string>& args) -> std::string
        {
            if (!m_ctx.progression)
                return "[TF] progression not ready";
            if (args.empty())
            {
                std::string out = "[TF] unlock keys:";
                for (const TFUnlockDef& def : TFUnlockTree::All())
                {
                    out += ' ';
                    out += def.key;
                }
                return out;
            }
            const TFUnlockResult r = m_ctx.progression->ServerTryUnlock(m_ctx.localPlayer, args[0]);
            switch (r)
            {
            case TFUnlockResult::Ok:
                return "[TF] unlocked '" + args[0] + "'";
            case TFUnlockResult::UnknownKey:
                return "[TF] refused: unknown key";
            case TFUnlockResult::AlreadyUnlocked:
                return "[TF] refused: already unlocked";
            case TFUnlockResult::RankTooLow:
                return "[TF] refused: rank too low";
            case TFUnlockResult::PrereqLocked:
                return "[TF] refused: prerequisite locked";
            default:
                return "[TF] refused: insufficient flux";
            }
        },
        "Buy an unlock-tree node (no args: list keys)", cat, "tf_unlock [key]");
}
