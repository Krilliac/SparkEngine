/**
 * @file TFVehicleSystemDrive.cpp
 * @brief W3 vehicles — authoritative driving model: the math-path StepVehicle
 *        (ground + Vulture VTOL flight), the StepVehicleJolt readback gate,
 *        the W13 damage-state movement penalties, the landed test and the ECS
 *        transform/health/deploy-pylon writeback. Split from
 *        TFVehicleSystem.cpp; shared internals live in
 *        TFVehicleSystemInternal.h.
 */
#include "Game/TFVehicleSystem.h"

#include "Data/TFDataTables.h"
#include "Game/TFComponents.h"
#include "Game/TFVehiclePhysics.h"
#include "Game/TFVehicleSystemInternal.h"
#include "Game/TFVisualUtils.h"
#include "Net/TFServerSim.h"

#include "Engine/ECS/Components.h"
#include "Spark/IEngineContext.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace Terrafront
{

    using namespace VehicleDetail;

    namespace
    {

        constexpr float kDriveDrag = 1.5f;      // 1/s throttle-off speed decay
        constexpr float kReverseFactor = 0.4f;  // reverse cap = topSpeed * this
        constexpr float kSteerRefSpeed = 2.0f;  // full steering authority above this
        constexpr float kTiltSampleM = 2.5f;    // terrain slope sample arm
        constexpr float kInputStaleSec = 0.25f; // driver input starvation -> coast

        // VTOL (Vulture) math-path mirrors of the TFVehiclePhysics.cpp Jolt feel
        // constants (same parity duplication convention as kDriveDrag above).
        constexpr float kVtolClimbRate = 8.0f;    // m/s climb at full lift
        constexpr float kVtolDescendRate = 6.0f;  // m/s descent at full negative lift
        constexpr float kVtolAutoDescend = 3.0f;  // m/s driverless / input-starved auto-land
        constexpr float kVtolCeilAGL = 120.0f;    // m max altitude above the terrain under the hull
        constexpr float kVtolWorldCeilY = 200.0f; // m hard absolute world ceiling
        constexpr float kVtolLeanPitch = 0.35f;   // rad nose-down visual lean at full throttle
        constexpr float kVtolLeanRoll = 0.45f;    // rad banking visual lean at full steer
        constexpr float kVtolLeanRate = 5.0f;     // 1/s lean approach speed (math path visual)
        constexpr float kVtolLandedAGL = 2.0f;    // m hull base above ground = landed (exit gate)

        // W13 damage-state performance degradation (server-authoritative). The
        // threshold is INTENTIONALLY the same numeric literal as
        // TFVehicleFx.cpp's kCriticalHpFrac (0.66f/0.33f tiers) so the client-
        // side smoke/fire tier and this movement penalty change state at the
        // same hp fraction (same cross-file duplication convention as the VTOL
        // math-path/Jolt-path constants above -- see DamageMovementMults). Only
        // the critical tier (<=33% hp) carries a movement penalty; the damaged
        // tier (33-66%, TFVehicleFx-only) is visuals with no mechanical effect.
        constexpr float kTFVehCriticalHpFrac = 0.33f;    // <=33% hp -> critical tier (speed/turn penalty)
        constexpr float kTFVehCriticalSpeedMult = 0.70f; // critical: ~30% top-speed loss
        constexpr float kTFVehCriticalTurnMult = 0.75f;  // critical: slower turn authority

    } // namespace

    // ---------------------------------------------------------------------------
    // Driving
    // ---------------------------------------------------------------------------

    void TFVehicleSystem::DamageMovementMults(const VehicleRec& v, float& outSpeedMult, float& outTurnMult) const
    {
        outSpeedMult = 1.0f;
        outTurnMult = 1.0f;
        if (v.maxHp <= 0.0f)
            return;
        if (v.hp / v.maxHp <= kTFVehCriticalHpFrac)
        {
            outSpeedMult = kTFVehCriticalSpeedMult;
            outTurnMult = kTFVehCriticalTurnMult;
        }
    }

    void TFVehicleSystem::StepVehicle(VehicleRec& v, const VehicleDef* def, float dt)
    {
        if (v.hp <= 0.0f)
            return;

        float speedMult = 1.0f, turnMult = 1.0f;
        DamageMovementMults(v, speedMult, turnMult);
        const float topSpeed = (def ? def->topSpeed : 10.0f) * speedMult;
        const float accel = def ? def->accel : 5.0f;
        const float turnRate = (def ? def->turnRate : 1.5f) * turnMult;

        const double now = (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;
        const bool driven = v.seats[0] != kInvalidPlayer && !v.deployed && (now - v.lastDriveInput) < kInputStaleSec;
        const bool vtol = v.vehId == VehicleId::Vulture;

        if (driven)
        {
            v.speed += v.throttle * accel * dt;
            if (vtol)
            {
                // A gunship yaws in place: full authority at any speed.
                v.yaw = QuantAim::WrapPi(v.yaw + v.steer * turnRate * dt);
            }
            else
            {
                // Steering authority scales in with speed; reversing mirrors the wheel.
                const float authority = std::clamp(std::fabs(v.speed) / kSteerRefSpeed, 0.0f, 1.0f);
                const float dir = (v.speed >= 0.0f) ? 1.0f : -1.0f;
                v.yaw = QuantAim::WrapPi(v.yaw + v.steer * turnRate * authority * dir * dt);
            }
        }
        else
        {
            const float keep = std::max(0.0f, 1.0f - kDriveDrag * dt);
            v.speed *= keep;
            if (std::fabs(v.speed) < 0.05f)
                v.speed = 0.0f;
        }
        v.speed = std::clamp(v.speed, -topSpeed * kReverseFactor, topSpeed);

        const float fwdX = std::sin(v.yaw);
        const float fwdZ = std::cos(v.yaw);
        v.pos[0] = std::clamp(v.pos[0] + fwdX * v.speed * dt, kWorldMin, kWorldMax);
        v.pos[2] = std::clamp(v.pos[2] + fwdZ * v.speed * dt, kWorldMin, kWorldMax);

        if (vtol)
        {
            // VTOL altitude: Jump/Crouch climb or descend, hold at lift 0,
            // auto-land when driverless; AGL ceiling + hard world ceiling.
            const float terrain = TerrainAt(v.pos[0], v.pos[2]);
            const float vy = driven ? (v.lift > 0.0f   ? v.lift * kVtolClimbRate
                                       : v.lift < 0.0f ? v.lift * kVtolDescendRate
                                                       : 0.0f)
                                    : -kVtolAutoDescend;
            const float ceilY = std::min(terrain + kVtolCeilAGL, kVtolWorldCeilY);
            v.pos[1] = std::clamp(v.pos[1] + vy * dt, terrain, std::max(terrain, ceilY));

            // Visual lean: forward tilt with throttle, banking with steer
            // (signs mirror the Jolt lean servo; positive pitch = nose down,
            // positive roll = lean left in this module's readback convention).
            const float k = std::min(1.0f, kVtolLeanRate * dt);
            const float targetPitch = driven ? v.throttle * kVtolLeanPitch : 0.0f;
            const float targetRoll = driven ? -v.steer * kVtolLeanRoll : 0.0f;
            v.pitch += (targetPitch - v.pitch) * k;
            v.roll += (targetRoll - v.roll) * k;
            return;
        }

        v.pos[1] = TerrainAt(v.pos[0], v.pos[2]);

        // Visual pitch/roll from the terrain slope under the hull.
        const float hF = TerrainAt(v.pos[0] + fwdX * kTiltSampleM, v.pos[2] + fwdZ * kTiltSampleM);
        const float hB = TerrainAt(v.pos[0] - fwdX * kTiltSampleM, v.pos[2] - fwdZ * kTiltSampleM);
        const float rX = fwdZ, rZ = -fwdX; // right vector
        const float hR = TerrainAt(v.pos[0] + rX * kTiltSampleM, v.pos[2] + rZ * kTiltSampleM);
        const float hL = TerrainAt(v.pos[0] - rX * kTiltSampleM, v.pos[2] - rZ * kTiltSampleM);
        v.pitch = std::atan2(hB - hF, 2.0f * kTiltSampleM); // nose up on uphill
        v.roll = std::atan2(hR - hL, 2.0f * kTiltSampleM);
    }

    bool TFVehicleSystem::StepVehicleJolt(VehicleRec& v, const VehicleDef* def)
    {
        if (!m_joltDrive)
            return false;
        if (v.hp <= 0.0f)
            return true; // destroyed hulls are erased synchronously; never simulate one

        // Same "driver + fresh input + not deployed" gate as the math path.
        const double now = (m_ctx && m_ctx->serverSim) ? m_ctx->serverSim->ServerTime() : m_clock;

        TFVehicleDriveState s;
        s.throttle = v.throttle;
        s.steer = v.steer;
        s.lift = v.lift;
        s.deployed = v.deployed;
        s.driven = v.seats[0] != kInvalidPlayer && !v.deployed && (now - v.lastDriveInput) < kInputStaleSec;
        if (def)
        {
            // Same critical-tier speed/turn penalty as the math path (see
            // DamageMovementMults) -- both driving paths run only on the
            // authority role, so applying the identical multiplier here keeps
            // them bit-for-bit consistent regardless of which one executes.
            float speedMult = 1.0f, turnMult = 1.0f;
            DamageMovementMults(v, speedMult, turnMult);
            s.topSpeed = def->topSpeed * speedMult;
            s.accel = def->accel;
            s.turnRate = def->turnRate * turnMult;
        }
        s.pos[0] = v.pos[0];
        s.pos[1] = v.pos[1];
        s.pos[2] = v.pos[2];
        s.yaw = v.yaw;
        s.pitch = v.pitch;
        s.roll = v.roll;
        s.speed = v.speed;

        if (!m_joltDrive->TickVehicle(v.entity, s))
            return false; // no hull body (attach failed) — math fallback

        v.pos[0] = s.pos[0];
        v.pos[1] = s.pos[1];
        v.pos[2] = s.pos[2];
        v.yaw = s.yaw;
        v.pitch = s.pitch;
        v.roll = s.roll;
        v.speed = s.speed;
        return true;
    }

    bool TFVehicleSystem::VehicleLanded(const VehicleRec& v) const
    {
        // Jolt hull: physics-aware clearance (a Vulture parked on a pad or roof
        // counts as landed). Math path: analytic terrain under the hull base.
        float clearance = 0.0f;
        if (m_joltDrive && m_joltDrive->GroundClearanceOf(v.entity, clearance))
            return clearance <= kVtolLandedAGL;
        return v.pos[1] - TerrainAt(v.pos[0], v.pos[2]) <= kVtolLandedAGL;
    }

    void TFVehicleSystem::WriteVehicleTransform(VehicleRec& v)
    {
        World* world = m_ctx->engine ? m_ctx->engine->GetWorld() : nullptr;
        if (!world || v.local == 0)
            return;
        const auto e = static_cast<EntityID>(v.local);
        if (!world->GetRegistry().valid(e))
            return;
        if (Transform* t = world->GetComponent<Transform>(e))
        {
            t->position = {v.pos[0], v.pos[1], v.pos[2]};
            t->rotation = {v.pitch * kRadToDeg, v.yaw * kRadToDeg, v.roll * kRadToDeg};
        }
        if (HealthComponent* hc = world->GetComponent<HealthComponent>(e))
        {
            hc->health = v.hp;
            hc->isDead = v.hp <= 0.0f;
        }
        if (TFVehicleComp* vc = world->GetComponent<TFVehicleComp>(e))
            std::memcpy(vc->seats, v.seats, sizeof(vc->seats));
        if (TFAegisDeployComp* dc = world->GetComponent<TFAegisDeployComp>(e))
        {
            const bool was = dc->active;
            dc->active = v.deployed;
            // Toggle the deployed-state pylon child on state change. The child is
            // hull-parented, so DestroyVehicle's child-sweep also cleans it up.
            TFVehicleComp* vc = world->GetComponent<TFVehicleComp>(e);
            const VehicleDef* def = vc ? DefOf(vc->vehId) : nullptr;
            if (v.deployed != was && def && !def->deployMesh.empty())
            {
                const std::string pylonPath = "Assets/" + def->deployMesh;
                auto& registry = world->GetRegistry();
                if (v.deployed)
                {
                    FactionId fac = FactionId::None;
                    if (TFFactionComp* fc = world->GetComponent<TFFactionComp>(e))
                        fac = fc->faction;
                    const auto pylon = world->CreateEntity("TF_AegisPylons");
                    Transform& pt = world->AddComponent<Transform>(pylon);
                    pt.parent = e;
                    MeshRenderer& pmr = world->AddComponent<MeshRenderer>(pylon);
                    pmr.meshPath = pylonPath;
                    pmr.materialPath = FactionStructureMaterial(*m_ctx, fac);
                    pmr.castShadows = true;
                }
                else
                {
                    std::vector<EntityID> pylons;
                    for (auto child : world->GetEntitiesWith<Transform, MeshRenderer>())
                    {
                        if (registry.get<Transform>(child).parent == e &&
                            registry.get<MeshRenderer>(child).meshPath == pylonPath)
                            pylons.push_back(child);
                    }
                    for (auto child : pylons)
                        world->DestroyEntity(child);
                }
            }
        }

        // W8: aim the turret rig (server visuals; clients mirror via 0x5448).
        ApplyTurretPose(v.rig, v.yaw, v.aimYaw, v.aimPitch);
    }

} // namespace Terrafront
