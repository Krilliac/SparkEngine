/**
 * @file TFVehicleSystemSeats.cpp
 * @brief W3 vehicles — seats: enter/exit with the one-tick exit latch, the
 *        W10 seat swap, seated driver/gunner input capture, ride-pose sync
 *        and the Aegis deploy-spawn toggle. Split from TFVehicleSystem.cpp;
 *        shared internals live in TFVehicleSystemInternal.h.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFPlayerSystem.h"
#include "Game/TFVehiclePhysics.h"
#include "Game/TFVehicleSystemInternal.h"
#include "Net/TFServerSim.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>

namespace Terrafront
{

    using namespace VehicleDetail;

    // ---------------------------------------------------------------------------
    // Seats
    // ---------------------------------------------------------------------------

    bool TFVehicleSystem::IsSeated(PlayerId player) const
    {
        if (m_seatOf.contains(player))
            return true;
        // Pure client: derive from the replicated seat tables.
        for (const auto& [entity, seats] : m_mirrorSeats)
            for (uint8_t i = 0; i < seats.seatCount && i < 8; ++i)
                if (seats.seats[i] == player)
                    return true;
        return false;
    }

    void TFVehicleSystem::ServerHandleSeatOp(PlayerId player, const TF_VehicleSeatOp& op, bool enter)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority() || !m_ctx->players)
            return;
        ++m_seatOps;

        if (!enter)
        {
            // Exit ignores op.vehicleEntity: you can only leave the seat you hold.
            auto it = m_seatOf.find(player);
            if (it == m_seatOf.end() || it->second.exiting)
                return;
            // VTOL: no bailing out mid-flight — the Vulture must be landed
            // (destruction still ejects everyone through UnseatPlayer directly).
            if (const VehicleRec* v = FindRec(it->second.vehicle);
                v && v->vehId == VehicleId::Vulture && !VehicleLanded(*v))
                return;
            UnseatPlayer(player, true);
            return;
        }

        // W10 seat swap: VehicleEnter while ALREADY seated in the named vehicle
        // moves the player to op.seatIndex without exiting (previously a silent
        // no-op, so the message reuse is backward-compatible). Validation is
        // strict — same vehicle, seat exists, not the current seat, and empty;
        // a failed swap keeps the current seat (no first-free fallback).
        if (auto sit = m_seatOf.find(player); sit != m_seatOf.end())
        {
            SeatRef& ref = sit->second;
            if (ref.exiting || op.vehicleEntity != ref.vehicle)
                return;
            VehicleRec* sv = FindRec(ref.vehicle);
            if (!sv || sv->hp <= 0.0f)
                return;
            if (op.seatIndex >= sv->seatCount || op.seatIndex == ref.seatIdx ||
                sv->seats[op.seatIndex] != kInvalidPlayer)
                return;
            if (ref.seatIdx < 8 && sv->seats[ref.seatIdx] == player)
                sv->seats[ref.seatIdx] = kInvalidPlayer;
            if (ref.seatIdx == 0)
            {
                // vacating the driver seat kills the cached drive inputs (same
                // as UnseatPlayer) — a Vulture starts its driverless auto-land.
                sv->throttle = 0.0f;
                sv->steer = 0.0f;
                sv->lift = 0.0f;
            }
            sv->seats[op.seatIndex] = player;
            sv->seatsDirty = true;
            ref.seatIdx = op.seatIndex;
            WriteSeatComp(player, sv->entity, op.seatIndex);
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u swapped to seat %u of vehicle %u", player,
                           static_cast<unsigned>(op.seatIndex), sv->entity);
            return;
        }

        VehicleRec* v = FindRec(op.vehicleEntity);
        if (!v || v->hp <= 0.0f)
            return;

        PawnInfo pawn{};
        if (!m_ctx->players->GetPawnByPlayer(player, pawn) || !pawn.alive)
            return;
        if (pawn.faction != v->faction)
            return; // no cross-faction theft in W3

        const float reach = VehicleRadius(v->vehId) + kTFVehEnterRangeM;
        if (Dist2XZ(pawn.pos, v->pos[0], v->pos[2]) > reach * reach)
            return;
        // VTOL: the XZ reach test alone would let a pawn board a Vulture hovering
        // 100 m straight above — flying hulls also need vertical proximity.
        if (v->vehId == VehicleId::Vulture && std::fabs(pawn.pos[1] - v->pos[1]) > reach)
            return;

        // Requested seat if free, else first free (driver seat 0 first).
        int seat = -1;
        if (op.seatIndex < v->seatCount && v->seats[op.seatIndex] == kInvalidPlayer)
            seat = op.seatIndex;
        else
            for (uint8_t i = 0; i < v->seatCount; ++i)
                if (v->seats[i] == kInvalidPlayer)
                {
                    seat = i;
                    break;
                }
        if (seat < 0)
            return; // full

        v->seats[seat] = player;
        v->seatsDirty = true;
        SeatRef ref;
        ref.vehicle = v->entity;
        ref.seatIdx = static_cast<uint8_t>(seat);
        m_seatOf[player] = ref;

        WriteSeatComp(player, v->entity, static_cast<uint8_t>(seat));

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] player %u entered vehicle %u seat %d", player, v->entity, seat);
    }

    void TFVehicleSystem::WriteSeatComp(PlayerId player, EntityId vehicle, uint8_t seatIdx)
    {
        // ECS mirror on the pawn (TFComponents contract) — shared by the enter
        // and W10 swap paths. No-op headless (no world) or with no live pawn.
        if (!m_ctx || !m_ctx->players)
            return;
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        PawnInfo pawn{};
        uint32_t local = 0;
        if (!world || !m_ctx->players->GetPawnByPlayer(player, pawn) ||
            !m_ctx->players->ResolveEntity(pawn.entity, local))
            return;
        const auto e = static_cast<EntityID>(local);
        if (!world->GetRegistry().valid(e))
            return;
        TFSeatComp& sc = world->HasComponent<TFSeatComp>(e) ? *world->GetComponent<TFSeatComp>(e)
                                                            : world->AddComponent<TFSeatComp>(e);
        sc.vehicle = vehicle;
        sc.seatIdx = seatIdx;
    }

    void TFVehicleSystem::UnseatPlayer(PlayerId player, bool placeBeside)
    {
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end())
            return;
        SeatRef& ref = it->second;
        VehicleRec* v = FindRec(ref.vehicle);
        if (v && ref.seatIdx < 8 && v->seats[ref.seatIdx] == player)
        {
            v->seats[ref.seatIdx] = kInvalidPlayer;
            v->seatsDirty = true;
            if (ref.seatIdx == 0)
            {
                v->throttle = 0.0f;
                v->steer = 0.0f;
                v->lift = 0.0f;
            }
        }

        // Drop the pawn's ECS seat marker.
        if (m_ctx && m_ctx->players && m_ctx->engine)
        {
            PawnInfo pawn{};
            uint32_t local = 0;
            if (m_ctx->players->GetPawnByPlayer(player, pawn) && m_ctx->players->ResolveEntity(pawn.entity, local))
            {
                World* world = m_ctx->engine->GetWorld();
                const auto e = static_cast<EntityID>(local);
                if (world && world->GetRegistry().valid(e) && world->HasComponent<TFSeatComp>(e))
                    world->RemoveComponent<TFSeatComp>(e);
            }
        }

        if (placeBeside && v)
        {
            // One-tick exit latch: IsSeated stays true until TFServerSim pulls the
            // exit placement through SyncSeatedPawn (see header contract).
            ref.exiting = true;
            ref.exitTicks = 0;
            ExitPosFor(*v, ref.seatIdx, ref.exitPos);
        }
        else
        {
            m_seatOf.erase(it);
        }
    }

    void TFVehicleSystem::ServerHandleSeatedInput(PlayerId player, const TF_ClientInput& input, float dt)
    {
        (void)dt; // integration happens once per fixed tick in StepVehicle
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end() || it->second.exiting)
            return;

        VehicleRec* v = FindRec(it->second.vehicle);
        if (!v)
            return;

        // W8 turret aim: the controller seat's view angles drive the turret.
        // Guard non-finite angles OURSELVES — TFServerSim's seated-path guard
        // runs after this call, so a poisoned viewYaw would reach WrapPi's
        // non-terminating loop here otherwise.
        if (static_cast<int>(it->second.seatIdx) == TurretControllerSeat(DefOf(v->vehId)) &&
            std::isfinite(input.viewYaw) && std::isfinite(input.viewPitch))
        {
            v->aimYaw = QuantAim::WrapPi(input.viewYaw);
            v->aimPitch = std::clamp(input.viewPitch, kTFTurretPitchMinRad, kTFTurretPitchMaxRad);
        }

        if (it->second.seatIdx != 0)
            return; // non-driver seats: aim captured above, fire via the weapon path

        v->throttle = std::clamp(static_cast<float>(input.moveY) / 127.0f, -1.0f, 1.0f);
        v->steer = std::clamp(static_cast<float>(input.moveX) / 127.0f, -1.0f, 1.0f);
        // Vertical axis for VTOL hulls: Jump climbs, Crouch descends (the seated
        // pawn is not walking, so these bits are otherwise unused while driving).
        v->lift = ((input.buttons & TFB_Jump) != 0 ? 1.0f : 0.0f) - ((input.buttons & TFB_Crouch) != 0 ? 1.0f : 0.0f);
        v->lastDriveInput = (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
    }

    void TFVehicleSystem::SyncSeatedPawn(PlayerId player, float outPos[3], float outVel[3])
    {
        auto it = m_seatOf.find(player);
        if (it == m_seatOf.end())
            return; // leave the caller's state untouched

        if (it->second.exiting)
        {
            outPos[0] = it->second.exitPos[0];
            outPos[1] = it->second.exitPos[1];
            outPos[2] = it->second.exitPos[2];
            outVel[0] = outVel[1] = outVel[2] = 0.0f;
            m_seatOf.erase(it); // latch consumed — walking resumes next tick
            return;
        }

        const VehicleRec* v = FindRec(it->second.vehicle);
        if (!v)
        {
            m_seatOf.erase(it); // vehicle vanished without an eject (defensive)
            return;
        }
        RidePos(*v, outPos);
        outVel[0] = std::sin(v->yaw) * v->speed;
        outVel[1] = 0.0f;
        outVel[2] = std::cos(v->yaw) * v->speed;
    }

    // ---------------------------------------------------------------------------
    // Aegis deploy-spawn
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::ServerHandleAegisDeploy(PlayerId player, const TF_AegisDeploy& msg)
    {
        if (!m_initialized || !m_ctx || !m_ctx->IsAuthority())
            return;

        VehicleRec* v = FindRec(msg.vehicleEntity);
        const VehicleDef* def = v ? DefOf(v->vehId) : nullptr;
        if (!v || !def || !def->hasDeploySpawn || v->hp <= 0.0f)
            return;
        if (v->seatCount == 0 || v->seats[0] != player)
            return; // driver only

        const bool wantDeploy = msg.deploy != 0;
        if (wantDeploy == v->deployed)
            return;
        if (wantDeploy && std::fabs(v->speed) > kTFVehDeployMaxSpeed)
            return; // must be stopped

        v->deployed = wantDeploy;
        if (wantDeploy)
            v->speed = 0.0f;
        v->seatsDirty = true; // seats message carries the deploy flag reliably
        if (m_joltDrive)
            m_joltDrive->SetDeployed(v->entity, wantDeploy); // freeze/unfreeze the hull

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] Aegis %u %s by player %u", v->entity,
                       wantDeploy ? "DEPLOYED" : "undeployed", player);
        if (m_events)
            m_events->Fire(EvAegisDeployed{v->entity, wantDeploy});
    }

    bool TFVehicleSystem::GetAegisSpawnPos(EntityId vehicle, FactionId faction, float out[3]) const
    {
        const VehicleRec* v = FindRec(vehicle);
        const VehicleDef* def = v ? DefOf(v->vehId) : nullptr;
        if (!v || !def || !def->hasDeploySpawn || !v->deployed || v->hp <= 0.0f)
            return false;
        if (v->faction != faction)
            return false;
        ExitPosFor(*v, 1, out); // spawn on the side pad, terrain height
        return true;
    }

    float TFVehicleSystem::AegisRespawnDelaySec() const
    {
        const VehicleDef* def = DefOf(VehicleId::Aegis);
        return (def && def->deployRespawnSec > 0.0f) ? def->deployRespawnSec : 5.0f;
    }

} // namespace Terrafront
