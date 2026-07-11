/**
 * @file TFVehiclePhysics.cpp
 * @brief Hover-rig force model: corner-spring suspension, drive/steer servo, grip,
 *        upright damping.
 *
 * Forces are applied each server fixed tick AFTER reading back the pose the world
 * step produced — the single per-process step is driven by TFServerSim, never here
 * (PhysicsSystem.h "module-driven stepping" contract). A deployed Aegis switches its
 * hull to kinematic, so it is immobile but still collidable. Each suspension corner
 * supports a quarter of the hull mass at half compression, so the hull rests at the
 * tuned hover height above the ground. Constants below are engine-side feel
 * constants; topSpeed/accel/turnRate balance still comes from vehicles.json.
 *
 * Rotation convention: PhysicsBody Get/SetRotation and PhysicsBodyDesc::rotation are
 * passed straight to JPH::Quat::sEulerAngles, i.e. RADIANS with R = Rz * Ry * Rx.
 * The body basis is reconstructed from those angles explicitly instead of trusting
 * GetTransform() row/column conventions.
 */
#include "Game/TFVehiclePhysics.h"

#include "World/TFWorldSetup.h"

#include "Physics/PhysicsSystem.h"
#include "Spark/IEngineContext.h"
#include "Utils/LogMacros.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace Terrafront
{

    namespace
    {

        constexpr float kGravity = 9.81f; // matches the engine world default
        constexpr float kWorldMin = 0.0f; // mirrors TFVehicleSystem world bounds
        constexpr float kWorldMax = 4096.0f;
        constexpr float kReverseFactor = 0.4f;  // reverse cap = topSpeed * this (math-path parity)
        constexpr float kSteerRefSpeed = 2.0f;  // full steering authority above this (math-path parity)
        constexpr float kDriveDrag = 1.5f;      // 1/s coast drag when input starves (math-path parity)
        constexpr float kLateralGrip = 4.0f;    // 1/s sideways-slip velocity kill
        constexpr float kSuspensionDamp = 3.0f; // 1/s vertical damping per suspension corner
        constexpr float kYawGain = 6.0f;        // 1/s yaw-rate servo stiffness
        constexpr float kUprightGain = 4.0f;    // 1/s^2 world-up righting (flip recovery)
        constexpr float kTiltDamp = 2.0f;       // 1/s pitch/roll angular damping
        constexpr float kCornerInset = 0.8f;    // suspension arms at 80% of the half extents
        constexpr float kRayStartUp = 0.5f;     // suspension cast origin lift above the corner
        constexpr float kRescueDepthM = 5.0f;   // tunnelled this far below ground -> teleport out

        // VTOL (Vulture) feel constants — keep in sync with the math-path mirrors
        // in TFVehicleSystem.cpp (same duplication convention as kDriveDrag etc.).
        constexpr float kVtolClimbRate = 8.0f;    // m/s target climb at full lift
        constexpr float kVtolDescendRate = 6.0f;  // m/s target descent at full negative lift
        constexpr float kVtolAutoDescend = 3.0f;  // m/s driverless / input-starved auto-land
        constexpr float kVtolVyGain = 3.0f;       // 1/s vertical-velocity servo stiffness
        constexpr float kVtolCeilAGL = 120.0f;    // m max altitude above the ground under the hull
        constexpr float kVtolWorldCeilY = 200.0f; // m hard absolute world ceiling
        constexpr float kVtolLeanPitch = 0.35f;   // rad nose-down lean at full throttle
        constexpr float kVtolLeanRoll = 0.45f;    // rad banking lean at full steer

        float WrapPi(float a)
        {
            while (a > 3.14159265f)
                a -= 6.2831853f;
            while (a < -3.14159265f)
                a += 6.2831853f;
            return a;
        }

    } // namespace

    TFVehiclePhysics::TFVehiclePhysics() = default;
    TFVehiclePhysics::~TFVehiclePhysics()
    {
        Shutdown();
    }

    bool TFVehiclePhysics::Initialize(TFGameContext& ctx)
    {
        m_ctx = &ctx;
        ::PhysicsSystem* physics = ctx.engine ? ctx.engine->GetPhysics() : nullptr;
        // GetJoltSystem() is null both in the no-Jolt stub build and before the
        // engine world exists — either way there is nothing to simulate against.
        if (!physics || !physics->GetJoltSystem())
        {
            m_physics = nullptr;
            SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] vehicle physics: no live Jolt world — math driving model");
            return false;
        }
        m_physics = physics;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] vehicle physics: Jolt live — rigid-body driving model");
        return true;
    }

    void TFVehiclePhysics::Shutdown()
    {
        if (m_physics)
        {
            for (auto& [entity, rec] : m_bodies)
            {
                if (rec.body)
                    m_physics->RemoveBody(rec.body);
            }
        }
        m_bodies.clear();
        m_physics = nullptr;
    }

    // ---------------------------------------------------------------------------
    // Hull archetypes (OBJ footprint approximations; mirrors VehicleRadius)
    // ---------------------------------------------------------------------------

    TFVehiclePhysics::Hull TFVehiclePhysics::HullOf(VehicleId id)
    {
        switch (id)
        {
        case VehicleId::Drifter:
            return {1.1f, 0.45f, 1.6f, 900.0f, 0.45f};
        case VehicleId::Aegis:
            return {2.2f, 0.90f, 3.2f, 5000.0f, 0.55f};
        case VehicleId::Ravager:
            return {1.8f, 0.75f, 2.6f, 3200.0f, 0.50f};
        case VehicleId::Vulture:
            return {2.0f, 0.60f, 2.8f, 1800.0f, 0.60f, /*vtol*/ true};
        default:
            return {};
        }
    }

    // ---------------------------------------------------------------------------
    // Body lifecycle
    // ---------------------------------------------------------------------------

    bool TFVehiclePhysics::AttachVehicle(EntityId vehicle, VehicleId vehId, const float basePos[3], float yaw,
                                         uint32_t localEntity)
    {
        if (!m_physics)
            return false;
        DetachVehicle(vehicle); // defensive: re-create for a reused id

        const Hull hull = HullOf(vehId);
        constexpr uint16_t kHullMask = CollisionLayers::WorldStatic | CollisionLayers::Player |
                                       CollisionLayers::Vehicle | CollisionLayers::Deployable;

        PhysicsBodyDesc desc;
        desc.type = PhysicsBodyType::Dynamic;
        desc.mass = hull.mass;
        desc.position = {basePos[0], basePos[1] + hull.hover + hull.halfY, basePos[2]};
        desc.rotation = {0.0f, yaw, 0.0f}; // radians (see file header)
        desc.shape.type = CollisionShapeType::Box;
        desc.shape.dimensions = {hull.halfX, hull.halfY, hull.halfZ};
        desc.material.friction = 0.4f;
        desc.material.restitution = 0.05f;
        desc.material.linearDamping = 0.05f;
        desc.material.angularDamping = 0.3f;
        desc.collisionGroup = CollisionLayers::Vehicle;
        desc.collisionMask = kHullMask;
        desc.maxAngularVelocity = 10.0f; // a hovercraft never needs to tumble faster
        desc.name = "TF_VehHull_" + std::to_string(vehicle);
        desc.entityId = localEntity;

        std::shared_ptr<PhysicsBody> body = m_physics->CreateBody(desc);
        if (!body)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] vehicle %u: hull body creation failed — math fallback",
                           vehicle);
            return false;
        }
        // Push the gameplay layer to the broadphase explicitly (CreateBody does not
        // forward the desc bitmasks to the PhysicsBody wrapper).
        body->SetCollisionGroup(CollisionLayers::Vehicle);
        body->SetCollisionMask(kHullMask);

        BodyRec rec;
        rec.body = std::move(body);
        rec.hull = hull;
        m_bodies[vehicle] = std::move(rec);
        return true;
    }

    void TFVehiclePhysics::DetachVehicle(EntityId vehicle)
    {
        auto it = m_bodies.find(vehicle);
        if (it == m_bodies.end())
            return;
        if (m_physics && it->second.body)
            m_physics->RemoveBody(it->second.body);
        m_bodies.erase(it);
    }

    void TFVehiclePhysics::SetDeployed(EntityId vehicle, bool deployed)
    {
        auto it = m_bodies.find(vehicle);
        if (it == m_bodies.end() || !it->second.body)
            return;
        BodyRec& rec = it->second;
        if (rec.kinematic == deployed)
            return;
        rec.kinematic = deployed;
        rec.body->SetLinearVelocity({0.0f, 0.0f, 0.0f});
        rec.body->SetAngularVelocity({0.0f, 0.0f, 0.0f});
        rec.body->SetKinematic(deployed);
    }

    // ---------------------------------------------------------------------------
    // Ground sampling
    // ---------------------------------------------------------------------------

    float TFVehiclePhysics::GroundHeightAt(float x, float y, float z, const PhysicsBody* ignore) const
    {
        float ground = (m_ctx && m_ctx->world) ? m_ctx->world->TerrainHeightAt(x, z) : 0.0f;
        if (m_physics)
        {
            // Static physics geometry (bridges, pads, structures) can stand taller
            // than the analytic terrain; a filtered ray keeps the hover on top of it.
            const float castLen = (y - ground) + 1.0f;
            if (castLen > 0.0f)
            {
                const RaycastHit hit = m_physics->RaycastFiltered({x, y, z}, {0.0f, -1.0f, 0.0f}, castLen,
                                                                  CollisionLayers::WorldStatic, ignore);
                if (hit.hasHit && hit.point.y > ground)
                    ground = hit.point.y;
            }
        }
        return ground;
    }

    bool TFVehiclePhysics::GroundClearanceOf(EntityId vehicle, float& outClearanceM) const
    {
        auto it = m_bodies.find(vehicle);
        if (!m_physics || it == m_bodies.end() || !it->second.body)
            return false;
        const PhysicsBody& body = *it->second.body;
        const XMFLOAT3 c = body.GetPosition();
        const float ground = GroundHeightAt(c.x, c.y, c.z, &body);
        outClearanceM = (c.y - it->second.hull.halfY) - ground;
        return true;
    }

    // ---------------------------------------------------------------------------
    // Per-tick simulation half
    // ---------------------------------------------------------------------------

    bool TFVehiclePhysics::TickVehicle(EntityId vehicle, TFVehicleDriveState& s)
    {
        auto it = m_bodies.find(vehicle);
        if (!m_physics || it == m_bodies.end() || !it->second.body)
            return false;
        BodyRec& rec = it->second;
        PhysicsBody& body = *rec.body;
        const Hull& hull = rec.hull;

        // ---- 1) read back the pose produced by the last world step ------------
        XMFLOAT3 c = body.GetPosition();
        XMFLOAT3 vel = body.GetLinearVelocity();
        const XMFLOAT3 angVel = body.GetAngularVelocity();

        // Body basis from the Jolt Euler angles (radians, R = Rz*Ry*Rx).
        const XMFLOAT3 e = body.GetRotation();
        const float cx = std::cos(e.x), sx = std::sin(e.x);
        const float cy = std::cos(e.y), sy = std::sin(e.y);
        const float cz = std::cos(e.z), sz = std::sin(e.z);
        const float fwd[3] = {cx * sy * cz + sx * sz, cx * sy * sz - sx * cz, cx * cy};
        const float up[3] = {sx * sy * cz - cx * sz, sx * sy * sz + cx * cz, sx * cy};
        const float right[3] = {cy * cz, cy * sz, -sy};

        // Horizontal drive frame; on a (near) vertical hull keep the last yaw.
        const float fh = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
        float fwdH[3];
        if (fh > 1.0e-4f)
        {
            fwdH[0] = fwd[0] / fh;
            fwdH[1] = 0.0f;
            fwdH[2] = fwd[2] / fh;
            s.yaw = WrapPi(std::atan2(fwd[0], fwd[2]));
        }
        else
        {
            fwdH[0] = std::sin(s.yaw);
            fwdH[1] = 0.0f;
            fwdH[2] = std::cos(s.yaw);
        }
        const float rightH[3] = {fwdH[2], 0.0f, -fwdH[0]};

        // World-bounds clamp (hard stop, math-path parity) + tunnelling rescue.
        const float baseGround = GroundHeightAt(c.x, c.y, c.z, &body);
        bool poked = false;
        XMFLOAT3 fixedPos = c;
        if (c.x < kWorldMin || c.x > kWorldMax || c.z < kWorldMin || c.z > kWorldMax)
        {
            fixedPos.x = std::clamp(c.x, kWorldMin, kWorldMax);
            fixedPos.z = std::clamp(c.z, kWorldMin, kWorldMax);
            vel.x = 0.0f;
            vel.z = 0.0f;
            poked = true;
        }
        if (c.y - hull.halfY < baseGround - kRescueDepthM)
        {
            fixedPos.y = baseGround + hull.hover + hull.halfY;
            vel.y = 0.0f;
            poked = true;
        }
        if (hull.vtol && c.y > kVtolWorldCeilY)
        {
            // Hard world ceiling (the soft AGL cap below normally stops climb first).
            fixedPos.y = kVtolWorldCeilY;
            vel.y = std::min(vel.y, 0.0f);
            poked = true;
        }
        if (poked)
        {
            body.SetPosition(fixedPos);
            body.SetLinearVelocity(vel);
            c = fixedPos;
        }

        const float fwdSpeed = vel.x * fwdH[0] + vel.z * fwdH[2];

        s.pos[0] = c.x;
        s.pos[1] = c.y - hull.halfY; // hull base, VehicleRec convention
        s.pos[2] = c.z;
        s.pitch = std::atan2(-fwd[1], fh > 1.0e-4f ? fh : 1.0e-4f);
        s.roll = std::atan2(right[1], std::sqrt(right[0] * right[0] + right[2] * right[2]));
        s.speed = fwdSpeed;

        // Deployed hull is kinematic: pose is frozen, queue no forces.
        if (rec.kinematic)
            return true;

        // ---- 2) queue forces for the NEXT step --------------------------------
        const float quarterWeight = hull.mass * kGravity * 0.25f;
        const float maxSuspLen = 2.0f * hull.hover;

        // Four corner springs (rest at half compression = hover clearance).
        for (int i = 0; i < 4; ++i)
        {
            const float lx = ((i & 1) != 0 ? 1.0f : -1.0f) * hull.halfX * kCornerInset;
            const float lz = ((i & 2) != 0 ? 1.0f : -1.0f) * hull.halfZ * kCornerInset;
            const float ly = -hull.halfY;
            const XMFLOAT3 off = {right[0] * lx + up[0] * ly + fwd[0] * lz, right[1] * lx + up[1] * ly + fwd[1] * lz,
                                  right[2] * lx + up[2] * ly + fwd[2] * lz};
            const float px = c.x + off.x;
            const float py = c.y + off.y;
            const float pz = c.z + off.z;
            const float groundY = GroundHeightAt(px, py + kRayStartUp, pz, &body);
            const float clearance = py - groundY;
            if (clearance >= maxSuspLen)
                continue; // corner in free fall — gravity handles it
            const float compress = std::clamp(1.0f - clearance / maxSuspLen, 0.0f, 1.0f);
            const float pointVelY = vel.y + angVel.z * off.x - angVel.x * off.z;
            float force = quarterWeight * 2.0f * compress - kSuspensionDamp * pointVelY * hull.mass * 0.25f;
            force = std::clamp(force, 0.0f, hull.mass * kGravity);
            body.ApplyForce({0.0f, force, 0.0f}, off);
        }

        // VTOL lift: servo the vertical velocity toward the lift input (hold
        // altitude at lift 0, auto-land when driverless). Near the ground with no
        // climb intent the corner springs take over so the hull settles at its
        // hover clearance = landed.
        if (hull.vtol)
        {
            const float agl = (c.y - hull.halfY) - baseGround;
            float targetVy = s.driven ? (s.lift > 0.0f   ? s.lift * kVtolClimbRate
                                         : s.lift < 0.0f ? s.lift * kVtolDescendRate
                                                         : 0.0f)
                                      : -kVtolAutoDescend;
            if (agl >= kVtolCeilAGL || c.y >= kVtolWorldCeilY)
                targetVy = std::min(targetVy, 0.0f); // soft ceiling: no more climb
            const bool settling = targetVy <= 0.0f && agl <= maxSuspLen;
            if (!settling)
            {
                float liftForce = hull.mass * (kGravity + (targetVy - vel.y) * kVtolVyGain);
                liftForce = std::clamp(liftForce, 0.0f, hull.mass * kGravity * 2.0f);
                body.ApplyForce({0.0f, liftForce, 0.0f});
            }
        }

        // Drive + steering servo (or coast drag when input starves).
        float targetYawRate = 0.0f;
        if (s.driven)
        {
            float drive = s.throttle * s.accel * hull.mass;
            if ((drive > 0.0f && fwdSpeed >= s.topSpeed) || (drive < 0.0f && fwdSpeed <= -s.topSpeed * kReverseFactor))
                drive = 0.0f;
            body.ApplyForce({fwdH[0] * drive, 0.0f, fwdH[2] * drive});

            if (hull.vtol)
            {
                // A gunship yaws in place: full authority at any speed, no
                // mirrored wheel when drifting backwards.
                targetYawRate = s.steer * s.turnRate;
            }
            else
            {
                const float authority = std::clamp(std::fabs(fwdSpeed) / kSteerRefSpeed, 0.0f, 1.0f);
                const float dir = (fwdSpeed >= 0.0f) ? 1.0f : -1.0f;
                targetYawRate = s.steer * s.turnRate * authority * dir;
            }
        }
        else
        {
            body.ApplyForce(
                {-fwdH[0] * fwdSpeed * hull.mass * kDriveDrag, 0.0f, -fwdH[2] * fwdSpeed * hull.mass * kDriveDrag});
        }

        const float yawInertia = hull.mass * (hull.halfX * hull.halfX + hull.halfZ * hull.halfZ) / 3.0f;
        body.ApplyTorque({0.0f, (targetYawRate - angVel.y) * yawInertia * kYawGain, 0.0f});

        // Lateral grip: kill sideways slip so the hull tracks its nose.
        const float latSpeed = vel.x * rightH[0] + vel.z * rightH[2];
        body.ApplyForce(
            {-rightH[0] * latSpeed * hull.mass * kLateralGrip, 0.0f, -rightH[2] * latSpeed * hull.mass * kLateralGrip});

        // Upright righting (flip recovery) + pitch/roll damping. Near the ground
        // the suspension dominates, so the hull still tilts with the terrain. The
        // servo rights the hull toward a target up vector — world up for ground
        // hulls; a driven VTOL leans it forward with throttle and sideways with
        // steer (banking), which is what produces the gunship's visual tilt.
        float tUp[3] = {0.0f, 1.0f, 0.0f};
        if (hull.vtol && s.driven)
        {
            const float lf = kVtolLeanPitch * s.throttle; // up leans toward the nose = nose-down tilt
            const float lr = kVtolLeanRoll * s.steer;     // up leans toward +right = bank into the turn
            tUp[0] = fwdH[0] * lf + rightH[0] * lr;
            tUp[2] = fwdH[2] * lf + rightH[2] * lr;
            const float len = std::sqrt(tUp[0] * tUp[0] + 1.0f + tUp[2] * tUp[2]);
            tUp[0] /= len;
            tUp[1] = 1.0f / len;
            tUp[2] /= len;
        }
        // Righting torque axis = up x targetUp (reduces to the old -up.z / +up.x
        // world-up form when tUp = (0,1,0)); yaw stays with the yaw servo above.
        const float rightingX = up[1] * tUp[2] - up[2] * tUp[1];
        const float rightingZ = up[0] * tUp[1] - up[1] * tUp[0];
        const float tiltInertia = hull.mass * (hull.halfY * hull.halfY + hull.halfZ * hull.halfZ) / 3.0f;
        body.ApplyTorque({rightingX * tiltInertia * kUprightGain - angVel.x * tiltInertia * kTiltDamp, 0.0f,
                          rightingZ * tiltInertia * kUprightGain - angVel.z * tiltInertia * kTiltDamp});
        return true;
    }

} // namespace Terrafront
