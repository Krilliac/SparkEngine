/**
 * @file TFCommandsHarness.cpp
 * @brief TF-110 multi-process harness commands (split part of TFCommands.cpp).
 *
 * Client verbs: tf_observe, tf_walk, tf_aim_at, tf_give_raw, tf_vehicle_buy,
 * tf_vehicle_seat. A headless client driven by an -exec script uses these to
 * move, aim, buy and board vehicles and send raw loadout ids through the SAME
 * client paths a player uses (TF_ClientInput via TFClientNet::PumpInput, the
 * TF_LoadoutChange / TF_VehPurchase / TF_VehicleSeatOp wire messages via
 * SendMsg); they grant no capability a modified client lacks, because the
 * server is the trust boundary.
 *
 * Authority verbs: tf_place_faction, tf_flux_floor, tf_damage_vehicles. A
 * dedicated server's -exec script is written before any client connects, so it
 * cannot name player ids; these address players by faction and are idempotent
 * (they act only on pawns still in the sanctuary, wallets below the floor, and
 * live vehicles), so the harness may repeat them across a window that absorbs
 * the clients' unknown start skew. They refuse to run on a pure client.
 *
 * tf_observe prints this process's view of the world (Game/TFObservation.h)
 * on every role so the harness can diff the server against each client; on the
 * authority it appends one "[TF-OBSERVE] player" line per live pawn with the
 * server-held saved loadout, flux, rank and kill tally the scenario verdicts
 * are checked against. A pure client holds no progression (TFProgressionSystem
 * is filled on the authority only), so its self line reads flux=0 rank=1.
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
#include "Net/TFRepProtocol.h"
#include "Net/TFServerSim.h"
#include "Persistence/TFPlayerMeta.h"
#include "World/TFRegionSystem.h"
#include "World/TFSanctuaryZone.h"
#include "World/TFWorldSetup.h"

#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
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

        /// tf_place_faction lines several same-faction pawns up this far apart
        /// so they never stack on one point.
        constexpr float kPlaceSpacingM = 2.0f;

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

        std::optional<VehicleId> ParseVehicleKind(const std::string& text)
        {
            const std::string kind = Lower(text);
            if (kind == "drifter")
                return VehicleId::Drifter;
            if (kind == "aegis")
                return VehicleId::Aegis;
            if (kind == "ravager")
                return VehicleId::Ravager;
            if (kind == "vulture")
                return VehicleId::Vulture;
            return std::nullopt;
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

        /// Authority only: "[TF-OBSERVE] player" lines, sorted by id, for every
        /// live pawn. A pure client holds no progression at all, so it prints
        /// none, and its self line carries the defaults (flux 0, rank 1).
        std::string FormatPlayerProgress(const TFGameContext& ctx)
        {
            std::string out;
            if (!ctx.IsAuthority() || !ctx.players || !ctx.progression)
                return out;
            std::vector<PlayerId> owners;
            ctx.players->ForEachAlivePawn([&owners](const PawnInfo& pawn) { owners.push_back(pawn.owner); });
            std::sort(owners.begin(), owners.end());
            for (const PlayerId id : owners)
            {
                const TFLoadout* loadout = ctx.progression->GetLoadout(id);
                const std::string primary = (loadout && !loadout->primary.empty()) ? loadout->primary : "default";
                uint32_t kills = 0;
                if (const auto* stats = ctx.progression->AllStats(id))
                {
                    for (const auto& entry : *stats)
                        kills += entry.second.kills;
                }
                out += std::format("\n[TF-OBSERVE] player id={} loadout={} flux={} rank={} kills={}", id, primary,
                                   ctx.progression->FluxOf(id), ctx.progression->RankOf(id), kills);
            }
            return out;
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

        float DistanceSq(const float a[3], const float b[3])
        {
            const float dx = a[0] - b[0];
            const float dy = a[1] - b[1];
            const float dz = a[2] - b[2];
            return dx * dx + dy * dy + dz * dz;
        }

        /// Nearest live pawn of another faction in this process's view.
        std::optional<PlayerId> NearestEnemy(const TFGameContext& ctx, const float eye[3])
        {
            PawnInfo self{};
            if (!ctx.players->GetPawnByPlayer(ctx.localPlayer, self))
                return std::nullopt;
            std::optional<PlayerId> nearest;
            float best = std::numeric_limits<float>::max();
            ctx.players->ForEachAlivePawn(
                [&](const PawnInfo& pawn)
                {
                    if (pawn.faction == self.faction || pawn.faction == FactionId::None)
                        return;
                    const float d2 = DistanceSq(pawn.pos, eye);
                    if (d2 < best)
                    {
                        best = d2;
                        nearest = pawn.owner;
                    }
                });
            return nearest;
        }

        /// Server-side guard shared by the authority verbs.
        bool IsServerInstance(const TFGameContext& ctx)
        {
            return ctx.IsAuthority() && ctx.serverSim && ctx.players;
        }

        /// Live pawns of `faction` that have passed the enter-world gate.
        std::vector<PawnInfo> EnteredPawnsOf(const TFGameContext& ctx, FactionId faction)
        {
            std::vector<PawnInfo> pawns;
            ctx.players->ForEachAlivePawn(
                [&](const PawnInfo& pawn)
                {
                    if (pawn.faction == faction && ctx.serverSim->IsEnteredWorld(pawn.owner))
                        pawns.push_back(pawn);
                });
            std::sort(pawns.begin(), pawns.end(),
                      [](const PawnInfo& a, const PawnInfo& b) { return a.owner < b.owner; });
            return pawns;
        }

        void RegisterObserveAndInput(Spark::SimpleConsole& console, TFGameContext* context, const char* cat)
        {
            console.RegisterCommand(
                "tf_observe",
                [context](const std::vector<std::string>&) -> std::string
                {
                    const std::string text =
                        FormatObservation(BuildObservation(*context)) + FormatPlayerProgress(*context);
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
                    // Seated, the same input stream drives the vehicle (TFServerSim
                    // forwards it to TFVehicleSystem::ServerHandleSeatedInput).
                    context->clientNet->SetScriptedMove(*forward, *right, *seconds);
                    return "[TF] scripted move set";
                },
                "TF-110 harness: walk (or drive) through the normal input path", cat,
                "tf_walk <forward> <right> <seconds>");

            console.RegisterCommand(
                "tf_aim_at",
                [context](const std::vector<std::string>& args) -> std::string
                {
                    if (args.size() != 1)
                        return "[TF] usage: tf_aim_at <playerId|enemy>";
                    if (!context->clientNet || !context->players || !context->HasLocalPlayer())
                        return "[TF] tf_aim_at: no local player on this instance";
                    float eye[3] = {};
                    if (!LocalEye(*context, eye))
                        return "[TF] tf_aim_at: the local pawn is not alive";

                    std::optional<PlayerId> target;
                    if (Lower(args[0]) == "enemy")
                    {
                        target = NearestEnemy(*context, eye);
                        if (!target)
                            return "[TF] tf_aim_at: no live enemy pawn in this view";
                    }
                    else if (const std::optional<unsigned long> id = ParseUnsigned(args[0], 0xFFFFFFFFul))
                    {
                        target = static_cast<PlayerId>(*id);
                    }
                    else
                    {
                        return "[TF] tf_aim_at: bad player id '" + args[0] + "'";
                    }

                    PawnInfo pawn{};
                    if (!context->players->GetPawnByPlayer(*target, pawn) || !pawn.alive)
                        return "[TF] tf_aim_at: player " + std::to_string(*target) + " has no live pawn in this view";
                    const float aimPoint[3] = {pawn.pos[0], pawn.pos[1] + kAimTorsoHeightM, pawn.pos[2]};
                    float yaw = 0.0f;
                    float pitch = 0.0f;
                    TFAimAngles(eye, aimPoint, yaw, pitch);
                    context->clientNet->SetViewAngles(yaw, pitch);
                    return "[TF] aiming at player " + std::to_string(*target);
                },
                "TF-110 harness: point the local view at a player's pawn (enemy = nearest other faction)", cat,
                "tf_aim_at <playerId|enemy>");

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

        void RegisterVehicleVerbs(Spark::SimpleConsole& console, TFGameContext* context, const char* cat)
        {
            console.RegisterCommand(
                "tf_vehicle_buy",
                [context](const std::vector<std::string>& args) -> std::string
                {
                    if (args.size() != 1)
                        return "[TF] usage: tf_vehicle_buy <drifter|aegis|ravager|vulture>";
                    const std::optional<VehicleId> kind = ParseVehicleKind(args[0]);
                    if (!kind)
                        return "[TF] tf_vehicle_buy: unknown vehicle '" + args[0] + "'";
                    // The purchase channel is socket-only; the authority buys in
                    // process through tf_vehicle.
                    if (context->IsAuthority())
                        return "[TF] tf_vehicle_buy: this instance is the authority - use tf_vehicle";
                    if (!ClientConnected(*context))
                        return "[TF] not connected - use tf_connect first";
                    TF_VehPurchase request{};
                    request.vehId = static_cast<uint8_t>(*kind);
                    context->clientNet->SendMsg(static_cast<TFMsg>(kTFVehMsg_Purchase), &request, sizeof(request));
                    return "[TF] vehicle purchase sent (server checks terminal, unlock and flux)";
                },
                "TF-110 harness: request a vehicle at a friendly terminal over the client purchase channel", cat,
                "tf_vehicle_buy <kind>");

            console.RegisterCommand(
                "tf_vehicle_seat",
                [context](const std::vector<std::string>& args) -> std::string
                {
                    const std::string op = args.size() == 1 ? Lower(args[0]) : std::string();
                    if (op != "enter" && op != "exit")
                        return "[TF] usage: tf_vehicle_seat <enter|exit>";
                    if (!ClientConnected(*context) || !context->vehicles || !context->players)
                        return "[TF] not connected - use tf_connect first";

                    TF_VehicleSeatOp seatOp{};
                    if (op == "exit")
                    {
                        EntityId vehicle = 0;
                        uint8_t seat = 0;
                        if (!context->vehicles->GetSeatOf(context->localPlayer, vehicle, seat))
                            return "[TF] tf_vehicle_seat: not seated";
                        seatOp.vehicleEntity = vehicle;
                        seatOp.seatIndex = seat;
                        context->clientNet->SendMsg(TFMsg::VehicleExit, &seatOp, sizeof(seatOp));
                        return "[TF] vehicle exit requested";
                    }

                    PawnInfo self{};
                    if (!context->players->GetPawnByPlayer(context->localPlayer, self) || !self.alive)
                        return "[TF] tf_vehicle_seat: the local pawn is not alive";
                    std::optional<EntityId> nearest;
                    float best = std::numeric_limits<float>::max();
                    context->vehicles->ForEachVehicle(
                        [&](const TFVehicleInfo& vehicle)
                        {
                            const float d2 = DistanceSq(vehicle.pos, self.pos);
                            if (d2 < best)
                            {
                                best = d2;
                                nearest = vehicle.entity;
                            }
                        });
                    if (!nearest)
                        return "[TF] tf_vehicle_seat: no vehicle in this view";
                    seatOp.vehicleEntity = *nearest;
                    seatOp.seatIndex = 0; // driver
                    context->clientNet->SendMsg(TFMsg::VehicleEnter, &seatOp, sizeof(seatOp));
                    return "[TF] driver seat of vehicle " + std::to_string(*nearest) + " requested";
                },
                "TF-110 harness: request the driver seat of the nearest vehicle, or leave the current seat", cat,
                "tf_vehicle_seat <enter|exit>");
        }

        void RegisterAuthorityVerbs(Spark::SimpleConsole& console, TFGameContext* context, const char* cat)
        {
            console.RegisterCommand(
                "tf_place_faction",
                [context](const std::vector<std::string>& args) -> std::string
                {
                    FactionId faction = FactionId::None;
                    if (args.size() != 3 || !ParseFaction(args[0], faction))
                        return "[TF] usage: tf_place_faction <mra|auc|hlx> <x> <z>";
                    const std::optional<float> x = ParseFiniteFloat(args[1]);
                    const std::optional<float> z = ParseFiniteFloat(args[2]);
                    if (!IsServerInstance(*context) || !context->world || !context->data || !context->data->IsLoaded())
                        return "[TF] tf_place_faction is authority-only";
                    const float size = context->data->GetContinent().sizeM;
                    if (!x || !z || *x < 0.0f || *z < 0.0f || *x > size || *z > size)
                        return "[TF] tf_place_faction: x and z must lie inside the continent";

                    // Only pawns still in the sanctuary move, so a repeated run
                    // never drags a pawn that already left (or fights back).
                    uint32_t placed = 0;
                    for (const PawnInfo& pawn : EnteredPawnsOf(*context, faction))
                    {
                        if (!TFTravel_IsInSanctuary(pawn.pos[0], pawn.pos[2]))
                            continue;
                        const float px = std::min(*x + kPlaceSpacingM * static_cast<float>(placed), size);
                        context->serverSim->TeleportPawn(pawn.owner, px, context->world->TerrainHeightAt(px, *z), *z);
                        ++placed;
                    }
                    return std::format("[TF] placed {} {} pawn(s) from the sanctuary at ({:.1f}, {:.1f})", placed,
                                       FactionTag(faction), *x, *z);
                },
                "TF-110 harness: move a faction's sanctuary pawns onto the continent (authority only)", cat,
                "tf_place_faction <mra|auc|hlx> <x> <z>");

            console.RegisterCommand(
                "tf_flux_floor",
                [context](const std::vector<std::string>& args) -> std::string
                {
                    FactionId faction = FactionId::None;
                    if (args.size() != 2 || !ParseFaction(args[0], faction))
                        return "[TF] usage: tf_flux_floor <mra|auc|hlx> <amount>";
                    const std::optional<unsigned long> amount = ParseUnsigned(args[1], 0xFFFFFFFFul);
                    if (!amount)
                        return "[TF] tf_flux_floor: bad amount '" + args[1] + "'";
                    if (!IsServerInstance(*context) || !context->progression)
                        return "[TF] tf_flux_floor is authority-only";

                    uint32_t raised = 0;
                    const uint32_t floor = static_cast<uint32_t>(*amount);
                    for (const PawnInfo& pawn : EnteredPawnsOf(*context, faction))
                    {
                        const uint32_t flux = context->progression->FluxOf(pawn.owner);
                        if (flux >= floor)
                            continue;
                        context->progression->ServerGrantFlux(pawn.owner, floor - flux);
                        ++raised;
                    }
                    return std::format("[TF] raised {} {} wallet(s) to {} flux", raised, FactionTag(faction), floor);
                },
                "TF-110 harness: top a faction's wallets up to a flux floor (authority only)", cat,
                "tf_flux_floor <mra|auc|hlx> <amount>");

            console.RegisterCommand(
                "tf_damage_vehicles",
                [context](const std::vector<std::string>& args) -> std::string
                {
                    const std::optional<float> amount =
                        args.size() == 1 ? ParseFiniteFloat(args[0]) : std::optional<float>();
                    if (!amount || *amount <= 0.0f)
                        return "[TF] usage: tf_damage_vehicles <amount > 0>";
                    if (!IsServerInstance(*context) || !context->vehicles)
                        return "[TF] tf_damage_vehicles is authority-only";

                    // Collect first: a lethal hit removes the record mid-iteration.
                    std::vector<EntityId> vehicles;
                    context->vehicles->ForEachVehicle([&vehicles](const TFVehicleInfo& vehicle)
                                                      { vehicles.push_back(vehicle.entity); });
                    for (const EntityId vehicle : vehicles)
                        context->vehicles->ServerDamageVehicle(vehicle, *amount, 0, kInvalidPlayer, kInvalidWeapon);
                    return std::format("[TF] applied {:.0f} damage to {} vehicle(s)", *amount, vehicles.size());
                },
                "TF-110 harness: damage every live vehicle through the authoritative damage path (authority only)", cat,
                "tf_damage_vehicles <amount>");
        }
    } // namespace

    void RegisterConsoleCommandsHarness(TFGameContext& ctx)
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        const char* cat = "TERRAFRONT";
        TFGameContext* context = &ctx;

        RegisterObserveAndInput(console, context, cat);
        RegisterVehicleVerbs(console, context, cat);
        RegisterAuthorityVerbs(console, context, cat);
    }

} // namespace Terrafront::CommandDetail
