/**
 * @file TFVehiclePhysics.h
 * @brief Jolt-backed server-side rigid-body simulation for TERRAFRONT hover vehicles.
 *
 * OWNERSHIP: belongs to the vehicles agent (TFVehicleSystem.h/.cpp lane). Owned and
 * driven exclusively by TFVehicleSystem on server-authority roles — one dynamic Jolt
 * box hull per purchased vehicle. Hover suspension comes from four downward corner
 * raycasts against static physics geometry with an analytic terrain-height fallback
 * (the module world has no terrain collision body; TFWorldSetup::TerrainHeightAt is
 * always the floor). Driver throttle/steer become a drive force and a yaw-rate servo
 * torque; lateral grip and coast drag mirror the arcade feel of the original math
 * driving model in TFVehicleSystem::StepVehicle. VTOL hulls (Vulture) add a vertical
 * thrust servo on top of the same rig: Jump/Crouch climb or descend toward a target
 * vertical velocity, the upright servo leans toward throttle/steer (forward tilt +
 * banking), altitude is capped ~120 m above the ground under the hull plus a hard
 * world ceiling, and a driverless hull auto-descends until the corner springs land it.
 *
 * When the engine has no live Jolt world (stub physics build), Initialize() reports
 * false and every per-vehicle call reports "not handled", so TFVehicleSystem keeps
 * the pure-math movement path and the module behaves exactly as before. The network
 * replication contract is untouched: this class only produces the same authoritative
 * pos/yaw/pitch/roll/speed state that math integration previously produced — the
 * 0x54F8+ wire formats, dirty checks and client mirror never see the rigid body.
 *
 * Stepping contract: this class NEVER advances the physics world. The single
 * per-process step (PhysicsSystem.h "module-driven stepping") is issued once per
 * authoritative fixed tick by TFServerSim; TickVehicle() then reads the stepped pose
 * back and queues forces for the NEXT step.
 */
#pragma once

#include "Core/TFTypes.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>

class PhysicsSystem; // Physics/PhysicsSystem.h (engine, global namespace)
class PhysicsBody;   // Physics/PhysicsBody.h

namespace Terrafront
{

    /// Flat per-tick marshalling mirror of the drive fields TFVehicleSystem's private
    /// VehicleRec exchanges with the rigid body (keeps VehicleRec private to the system).
    struct TFVehicleDriveState
    {
        // -- inputs (server-authoritative, cached from the driver's TF_ClientInput) --
        float throttle = 0.0f; ///< [-1, 1] forward/back
        float steer = 0.0f;    ///< [-1, 1] steering wheel
        float lift = 0.0f;     ///< [-1, 1] climb/descend (VTOL hulls only; Jump/Crouch bits)
        bool driven = false;   ///< driver seated + fresh input + not deployed
        bool deployed = false; ///< Aegis deployed: hull frozen kinematic, no forces
        // -- vehicles.json balance (engine-side feel constants live in the .cpp) --
        float topSpeed = 10.0f; ///< m/s forward cap
        float accel = 5.0f;     ///< m/s^2 drive acceleration
        float turnRate = 1.5f;  ///< rad/s steering authority
        // -- in/out authoritative pose (radians; pos = hull base, VehicleRec convention) --
        float pos[3] = {0.0f, 0.0f, 0.0f};
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        float speed = 0.0f; ///< signed forward speed, m/s
    };

    class TFVehiclePhysics
    {
      public:
        TFVehiclePhysics();
        ~TFVehiclePhysics();

        /// Caches the engine PhysicsSystem. Returns false — and the instance stays
        /// inert — when there is no live Jolt world (stub build / physics missing).
        bool Initialize(TFGameContext& ctx);
        void Shutdown();

        /// True when a live Jolt world backs this instance.
        bool Active() const { return m_physics != nullptr; }

        /// Server: create the vehicle's dynamic hull body at the spawn pose
        /// (`basePos` = hull base like VehicleRec.pos; yaw radians). Returns false
        /// when unavailable — the caller keeps the math path for this vehicle.
        bool AttachVehicle(EntityId vehicle, VehicleId vehId, const float basePos[3], float yaw, uint32_t localEntity);
        void DetachVehicle(EntityId vehicle);

        /// Aegis deploy toggle: deployed hulls freeze kinematic (still collidable).
        void SetDeployed(EntityId vehicle, bool deployed);

        /// Authority fixed tick, AFTER the world step (TFServerSim owns StepFixed):
        /// reads the stepped pose back into `s`, then applies suspension/drive/grip
        /// forces for the next step. Returns false when this vehicle has no body —
        /// the caller falls back to math integration for it.
        bool TickVehicle(EntityId vehicle, TFVehicleDriveState& s);

        /// Server: clearance of the hull BASE above the ground under it (analytic
        /// terrain raised by static physics geometry — a Vulture parked on a pad
        /// or roof counts as landed). False when this vehicle has no body; the
        /// caller falls back to a pure-terrain test.
        bool GroundClearanceOf(EntityId vehicle, float& outClearanceM) const;

        /// Debug: number of live hull bodies.
        size_t BodyCount() const { return m_bodies.size(); }

      private:
        /// Per-archetype hull tuning (footprints, not balance data — mirrors the
        /// bounding-sphere convention of TFVehicleSystem::VehicleRadius).
        struct Hull
        {
            float halfX = 1.5f; ///< box half width (m)
            float halfY = 0.6f; ///< box half height (m)
            float halfZ = 2.0f; ///< box half length (m)
            float mass = 1500.0f;
            float hover = 0.5f; ///< rest clearance of the hull base above ground (m)
            bool vtol = false;  ///< Vulture: lift thrust + lean servo instead of ground steering
        };
        struct BodyRec
        {
            std::shared_ptr<PhysicsBody> body;
            Hull hull{};
            bool kinematic = false;
        };

        static Hull HullOf(VehicleId id);

        /// Ground height under (x, z): analytic terrain, raised by a filtered
        /// raycast when static physics geometry stands taller. `y` = cast origin.
        float GroundHeightAt(float x, float y, float z, const PhysicsBody* ignore) const;

        TFGameContext* m_ctx{nullptr};
        ::PhysicsSystem* m_physics{nullptr}; ///< non-owning; null = Jolt absent
        std::unordered_map<EntityId, BodyRec> m_bodies;
    };

} // namespace Terrafront
