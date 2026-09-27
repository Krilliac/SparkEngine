/**
 * @file TFCommandsHarness.cpp
 * @brief TF-110 multi-process harness commands (split part of TFCommands.cpp):
 *        tf_observe, tf_walk, tf_aim_at, tf_give_raw.
 *
 * A headless client driven by an -exec script uses these to move, aim and
 * send raw loadout ids through the SAME client paths a player uses
 * (TF_ClientInput via TFClientNet::PumpInput, TF_LoadoutChange via SendMsg);
 * they grant no capability a modified client lacks, because the server is the
 * trust boundary. tf_observe prints this process's view of the world
 * (Game/TFObservation.h) on every role so the harness can diff the server
 * against each client.
 */

#include "Console/TFCommandsInternal.h"
#include "Data/TFDataTables.h"
#include "Game/TFObservation.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFProgressionSystem.h"
#include "Game/TFVehicleSystem.h"
#include "Game/TFWeaponMath.h"
#include "Net/TFClientNet.h"
#include "Net/TFNetProtocol.h"
#include "Net/TFServerSim.h"
#include "Persistence/TFPlayerMeta.h"
#include "World/TFRegionSystem.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace Terrafront::CommandDetail
{
    namespace
    {
        /// Aim point above the target's feet: torso height, inside the body
        /// hitbox for every class.
        constexpr float kAimTorsoHeightM = 1.2f;

        std::optional<float> ParseFiniteFloat(const std::string& text)
        {
            if (text.empty())
                return std::nullopt;
            char* end = nullptr;
            errno = 0;
            const float value = std::strtof(text.c_str(), &end);
            if (errno != 0 || end != text.c_str() + text.size() || !std::isfinite(value))
                return std::nullopt;
            return value;
        }

        std::optional<unsigned long> ParseUnsigned(const std::string& text, unsigned long maxValue)
        {
            if (text.empty() || text[0] == '-')
                return std::nullopt;
            char* end = nullptr;
            errno = 0;
            const unsigned long value = std::strtoul(text.c_str(), &end, 0);
            if (errno != 0 || end != text.c_str() + text.size() || value > maxValue)
                return std::nullopt;
            return value;
        }

        int16_t ToInt16(float value)
        {
            return static_cast<int16_t>(std::clamp(std::lround(value), -32768L, 32767L));
        }

        TFObservation BuildObservation(const TFGameContext& ctx)
        {
            TFObservation obs;
            obs.role = ctx.IsAuthority() ? (ctx.HasLocalPlayer() ? "host" : "server") : "client";
            if (ctx.serverSim)
                obs.clock = ctx.serverSim->ServerTime();
            else if (ctx.clientNet)
                obs.clock = ctx.clientNet->ClockSec();
            obs.self = ctx.localPlayer;
            if (ctx.data && ctx.data->IsLoaded())
                obs.continentKey = ctx.data->GetContinent().key;

            if (ctx.players)
            {
                ctx.players->ForEachAlivePawn(
                    [&obs](const PawnInfo& pawn)
                    {
                        TFObservedPawn p;
                        p.id = pawn.owner;
                        p.faction = static_cast<uint8_t>(pawn.faction);
                        p.cls = static_cast<uint8_t>(pawn.cls);
                        p.health = ToInt16(pawn.health);
                        std::copy(pawn.pos, pawn.pos + 3, p.pos);
                        obs.pawns.push_back(p);
                    });
            }
            if (ctx.regions)
            {
                for (uint32_t region = 0; region < ctx.regions->RegionCount(); ++region)
                {
                    const RegionId id = static_cast<RegionId>(region);
                    obs.regionOwners.emplace_back(id, static_cast<uint8_t>(ctx.regions->OwnerOf(id)));
                }
            }
            if (ctx.vehicles)
            {
                ctx.vehicles->ForEachVehicle(
                    [&obs](const TFVehicleInfo& vehicle)
                    {
                        TFObservedVehicle v;
                        v.netId = vehicle.entity;
                        v.def = static_cast<uint16_t>(vehicle.vehId);
                        v.driver = vehicle.seatCount > 0 ? vehicle.seats[0] : kInvalidPlayer;
                        v.hp = ToInt16(vehicle.hp);
                        std::copy(vehicle.pos, vehicle.pos + 3, v.pos);
                        obs.vehicles.push_back(v);
                    });
            }
            if (ctx.progression && obs.self != kInvalidPlayer)
            {
                obs.flux = ctx.progression->FluxOf(obs.self);
                obs.rank = ctx.progression->RankOf(obs.self);
                if (const TFLoadout* loadout = ctx.progression->GetLoadout(obs.self))
                    obs.loadoutPrimary = loadout->primary;
            }
            return obs;
        }

        /// The local player's eye position: predicted on a pure client, the
        /// authoritative pawn on a listen host.
        bool LocalEye(const TFGameContext& ctx, float outEye[3])
        {
            float vel[3] = {};
            float yaw = 0.0f;
            float pitch = 0.0f;
            if (!ctx.clientNet->GetPredictedLocalState(outEye, vel, yaw, pitch))
            {
                PawnInfo self{};
                if (!ctx.players || !ctx.players->GetPawnByPlayer(ctx.localPlayer, self) || !self.alive)
                    return false;
                std::copy(self.pos, self.pos + 3, outEye);
            }
            outEye[1] += WeaponMath::kEyeHeightM;
            return true;
        }
    } // namespace

    void RegisterConsoleCommandsHarness(TFGameContext& ctx)
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        const char* cat = "TERRAFRONT";
        TFGameContext* context = &ctx;

        console.RegisterCommand(
            "tf_observe",
            [context](const std::vector<std::string>&) -> std::string
            {
                const std::string text = FormatObservation(BuildObservation(*context));
                SPARK_LOG_INFO(Spark::LogCategory::Game, "%s", text.c_str());
                return text;
            },
            "TF-110 harness: print this process's world view as sorted [TF-OBSERVE] lines", cat, "tf_observe");

        console.RegisterCommand(
            "tf_walk",
            [context](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() != 3)
                    return "[TF] usage: tf_walk <forward -1..1> <right -1..1> <seconds>";
                const std::optional<float> forward = ParseFiniteFloat(args[0]);
                const std::optional<float> right = ParseFiniteFloat(args[1]);
                const std::optional<float> seconds = ParseFiniteFloat(args[2]);
                if (!forward || !right || !seconds || *seconds < 0.0f)
                    return "[TF] tf_walk: forward/right must be numbers and seconds >= 0";
                if (!context->clientNet || !context->HasLocalPlayer())
                    return "[TF] tf_walk: no local player on this instance";
                context->clientNet->SetScriptedMove(*forward, *right, *seconds);
                return "[TF] scripted move set";
            },
            "TF-110 harness: walk the local pawn through the normal input path", cat,
            "tf_walk <forward> <right> <seconds>");

        console.RegisterCommand(
            "tf_aim_at",
            [context](const std::vector<std::string>& args) -> std::string
            {
                if (args.size() != 1)
                    return "[TF] usage: tf_aim_at <playerId>";
                const std::optional<unsigned long> target = ParseUnsigned(args[0], 0xFFFFFFFFul);
                if (!target)
                    return "[TF] tf_aim_at: bad player id '" + args[0] + "'";
                if (!context->clientNet || !context->players || !context->HasLocalPlayer())
                    return "[TF] tf_aim_at: no local player on this instance";
                PawnInfo pawn{};
                if (!context->players->GetPawnByPlayer(static_cast<PlayerId>(*target), pawn) || !pawn.alive)
                    return "[TF] tf_aim_at: player " + args[0] + " has no live pawn in this view";
                float eye[3] = {};
                if (!LocalEye(*context, eye))
                    return "[TF] tf_aim_at: the local pawn is not alive";
                const float aimPoint[3] = {pawn.pos[0], pawn.pos[1] + kAimTorsoHeightM, pawn.pos[2]};
                float yaw = 0.0f;
                float pitch = 0.0f;
                TFAimAngles(eye, aimPoint, yaw, pitch);
                context->clientNet->SetViewAngles(yaw, pitch);
                return "[TF] aiming at player " + args[0];
            },
            "TF-110 harness: point the local view at a player's pawn", cat, "tf_aim_at <playerId>");

        console.RegisterCommand(
            "tf_give_raw",
            [context](const std::vector<std::string>& args) -> std::string
            {
                if (args.empty() || args.size() > 3)
                    return "[TF] usage: tf_give_raw <primaryId> [secondaryId] [toolId]";
                uint16_t ids[3] = {kInvalidWeapon, kInvalidWeapon, kInvalidWeapon};
                for (size_t i = 0; i < args.size(); ++i)
                {
                    const std::optional<unsigned long> id = ParseUnsigned(args[i], 0xFFFFul);
                    if (!id)
                        return "[TF] tf_give_raw: bad weapon id '" + args[i] + "'";
                    ids[i] = static_cast<uint16_t>(*id);
                }
                if (!ClientConnected(*context))
                    return "[TF] not connected - use tf_host or tf_connect first";
                TF_LoadoutChange change{};
                PawnInfo self{};
                if (context->players && context->players->GetPawnByPlayer(context->localPlayer, self))
                    change.classId = static_cast<uint8_t>(self.cls);
                change.primary = ids[0];
                change.secondary = ids[1];
                change.tool = ids[2];
                context->clientNet->SendMsg(TFMsg::LoadoutChange, &change, sizeof(change));
                return "[TF] raw loadout change sent";
            },
            "TF-110 harness: send TF_LoadoutChange with raw weapon ids (server validates)", cat,
            "tf_give_raw <primaryId> [secondaryId] [toolId]");
    }

} // namespace Terrafront::CommandDetail
