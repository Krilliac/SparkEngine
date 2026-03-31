/**
 * @file MovementSystem.h
 * @brief Priority-based movement generator stack (TC MotionMaster-inspired)
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a per-entity movement controller that manages a stack of movement
 * generators in priority slots. Higher-priority slots override lower ones;
 * when a higher-priority generator completes, the lower-priority one resumes.
 *
 * Inspired by TrinityCore's MotionMaster: each AI entity has a MotionController
 * with four priority slots (Default, Motion, Active, Controlled). The
 * MovementSystem ECS system ticks all active controllers each frame.
 *
 * ## Slot priority (highest overrides lowest)
 *
 * | Slot       | Priority | Typical generators           |
 * |------------|----------|------------------------------|
 * | Controlled | 3        | Stun, root, knockback        |
 * | Active     | 2        | Flee, charge, scripted moves |
 * | Motion     | 1        | Chase, follow, waypoint      |
 * | Default    | 0        | Idle, random wander, patrol  |
 *
 * ## Usage
 * @code
 *   auto& moveSys = Spark::AI::MovementSystem::GetInstance();
 *   moveSys.Initialize();
 *
 *   // Set patrol as the default movement
 *   moveSys.MovePatrol(guardEntity, patrolWaypoints, true);
 *
 *   // When combat starts, chase overrides patrol in the Motion slot
 *   moveSys.MoveChase(guardEntity, playerEntity, 1.0f, 3.0f);
 *
 *   // When chase completes (target dead), patrol resumes automatically
 *   moveSys.StopMovement(guardEntity, MovementSlot::Motion);
 * @endcode
 *
 * @see AISystem.h (behavior trees decide which movement to request)
 * @see SteeringBehaviors.h (low-level steering used by some generators)
 */

#pragma once

#include "../../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class World; // Forward-declare ECS World (global scope)

namespace Spark::AI
{

    using EntityID = uint32_t;

    // =========================================================================
    // Movement Types and Slots
    // =========================================================================

    /** @brief Movement generator types identifying what kind of movement is active. */
    enum class MovementType : uint8_t
    {
        Idle,
        RandomWander,
        Waypoint,
        Chase,
        Follow,
        Flee,
        Point, ///< Move to a specific world-space point
        Home,  ///< Return to spawn position
        Patrol,
        Charge, ///< Fast dash toward a target
        Spline, ///< Follow a Catmull-Rom spline curve
        Count
    };

    /**
     * @brief Priority slots for movement generators.
     *
     * Higher-numbered slots override lower ones. When the generator in a
     * higher slot completes or is cleared, the lower slot's generator resumes.
     */
    enum class MovementSlot : uint8_t
    {
        Default = 0,   ///< Idle, RandomWander, Patrol
        Motion = 1,    ///< Chase, Follow, Waypoint
        Active = 2,    ///< Flee, Charge, scripted movement
        Controlled = 3 ///< Stun, root, knockback (cannot be overridden by AI)
    };

    static constexpr int kMovementSlotCount = 4;

    // =========================================================================
    // Movement Generator Interface
    // =========================================================================

    /**
     * @brief Base class for movement generators.
     *
     * Each generator drives one type of movement. It is placed into a
     * MotionController slot and ticked each frame until it completes.
     */
    struct IMovementGenerator
    {
        virtual ~IMovementGenerator() = default;

        virtual MovementType GetType() const = 0;
        virtual MovementSlot GetSlot() const = 0;
        virtual const char* GetName() const = 0;

        /** @brief Called once when the generator is placed into a slot. */
        virtual void Initialize(EntityID entity) = 0;

        /**
         * @brief Called each frame while this generator is the active one.
         * @return false when the movement is done (generator will be cleared).
         */
        virtual bool Update(EntityID entity, float deltaTime) = 0;

        /** @brief Called when the generator is removed from its slot. */
        virtual void Finalize(EntityID entity) = 0;

        /** @brief Reset the generator to its initial state. */
        virtual void Reset(EntityID entity) = 0;
    };

    // =========================================================================
    // Concrete Generators
    // =========================================================================

    /** @brief Does nothing -- entity stands still. */
    struct IdleGenerator : IMovementGenerator
    {
        MovementType GetType() const override { return MovementType::Idle; }
        MovementSlot GetSlot() const override { return MovementSlot::Default; }
        const char* GetName() const override { return "Idle"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Wander randomly within a radius, pausing between moves. */
    struct RandomWanderGenerator : IMovementGenerator
    {
        float radius = 10.0f;
        float waitTime = 3.0f;
        float waitTimer = 0.0f;
        float targetX = 0.0f;
        float targetY = 0.0f;
        float targetZ = 0.0f;
        float originX = 0.0f;
        float originY = 0.0f;
        float originZ = 0.0f;
        bool hasTarget = false;

        MovementType GetType() const override { return MovementType::RandomWander; }
        MovementSlot GetSlot() const override { return MovementSlot::Default; }
        const char* GetName() const override { return "RandomWander"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Chase a target entity, staying within min/max distance. */
    struct ChaseGenerator : IMovementGenerator
    {
        EntityID target = 0;
        float minDist = 0.5f;
        float maxDist = 2.0f;

        MovementType GetType() const override { return MovementType::Chase; }
        MovementSlot GetSlot() const override { return MovementSlot::Motion; }
        const char* GetName() const override { return "Chase"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Follow a target entity at a fixed offset. */
    struct FollowGenerator : IMovementGenerator
    {
        EntityID target = 0;
        float distance = 3.0f;
        float angle = 0.0f;

        MovementType GetType() const override { return MovementType::Follow; }
        MovementSlot GetSlot() const override { return MovementSlot::Motion; }
        const char* GetName() const override { return "Follow"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Flee from a threat entity for a duration. */
    struct FleeGenerator : IMovementGenerator
    {
        EntityID threatEntity = 0;
        float fleeDistance = 20.0f;
        float duration = 5.0f;
        float elapsed = 0.0f;

        MovementType GetType() const override { return MovementType::Flee; }
        MovementSlot GetSlot() const override { return MovementSlot::Active; }
        const char* GetName() const override { return "Flee"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Move to a specific world-space point. */
    struct PointGenerator : IMovementGenerator
    {
        float targetX = 0.0f;
        float targetY = 0.0f;
        float targetZ = 0.0f;
        float speed = 5.0f;

        MovementType GetType() const override { return MovementType::Point; }
        MovementSlot GetSlot() const override { return MovementSlot::Motion; }
        const char* GetName() const override { return "Point"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Patrol along a series of waypoints. */
    struct PatrolGenerator : IMovementGenerator
    {
        std::vector<std::array<float, 3>> waypoints;
        int currentIndex = 0;
        bool loop = true;
        float speed = 5.0f;

        MovementType GetType() const override { return MovementType::Patrol; }
        MovementSlot GetSlot() const override { return MovementSlot::Default; }
        const char* GetName() const override { return "Patrol"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Return to spawn/home position. */
    struct HomeGenerator : IMovementGenerator
    {
        float homeX = 0.0f;
        float homeY = 0.0f;
        float homeZ = 0.0f;
        float speed = 5.0f;

        MovementType GetType() const override { return MovementType::Home; }
        MovementSlot GetSlot() const override { return MovementSlot::Motion; }
        const char* GetName() const override { return "Home"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Fast dash toward a target entity over a short duration. */
    struct ChargeGenerator : IMovementGenerator
    {
        EntityID target = 0;
        float speed = 20.0f;      ///< Charge speed (faster than normal movement).
        float duration = 0.5f;    ///< Maximum charge duration (seconds).
        float elapsed = 0.0f;     ///< Time spent charging so far.
        float arrivalDist = 1.0f; ///< Stop when this close to target.

        MovementType GetType() const override { return MovementType::Charge; }
        MovementSlot GetSlot() const override { return MovementSlot::Active; }
        const char* GetName() const override { return "Charge"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Follow a Catmull-Rom spline path. */
    struct SplineGenerator : IMovementGenerator
    {
        std::vector<std::array<float, 3>> controlPoints; ///< Spline control points.
        float speed = 5.0f;
        float t = 0.0f;    ///< Normalized parameter [0, 1] along the spline.
        bool loop = false; ///< If true, wrap around when reaching the end.
        bool completed = false;

        MovementType GetType() const override { return MovementType::Spline; }
        MovementSlot GetSlot() const override { return MovementSlot::Motion; }
        const char* GetName() const override { return "Spline"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    /** @brief Move to a single waypoint and complete. */
    struct WaypointGenerator : IMovementGenerator
    {
        float targetX = 0.0f;
        float targetY = 0.0f;
        float targetZ = 0.0f;
        float speed = 5.0f;
        float arrivalThreshold = 0.3f; ///< Distance at which the waypoint is considered reached.

        MovementType GetType() const override { return MovementType::Waypoint; }
        MovementSlot GetSlot() const override { return MovementSlot::Motion; }
        const char* GetName() const override { return "Waypoint"; }
        void Initialize(EntityID entity) override;
        bool Update(EntityID entity, float deltaTime) override;
        void Finalize(EntityID entity) override;
        void Reset(EntityID entity) override;
    };

    // =========================================================================
    // MotionController
    // =========================================================================

    /**
     * @brief Per-entity movement controller with priority-based generator stack.
     *
     * Each entity has one MotionController. It holds up to four generators
     * (one per MovementSlot). The highest-priority non-null generator is the
     * active one and receives Update() calls. When it completes, the next
     * lower-priority generator resumes automatically.
     */
    class MotionController
    {
      public:
        /** @brief Place a generator into its declared slot, replacing any existing one. */
        void SetGenerator(std::unique_ptr<IMovementGenerator> generator);

        /** @brief Remove the generator in a specific slot. */
        void ClearSlot(MovementSlot slot);

        /** @brief Remove all generators. */
        void ClearAll();

        /** @brief Get the currently active (highest-priority) generator, or nullptr. */
        IMovementGenerator* GetActiveGenerator() const;

        /** @brief Get the movement type of the active generator (Idle if none). */
        MovementType GetActiveMovementType() const;

        /** @brief Tick the active generator. */
        void Update(EntityID entity, float deltaTime);

        /** @brief Check if a specific slot has a generator. */
        bool HasMovement(MovementSlot slot) const;

      private:
        std::array<std::unique_ptr<IMovementGenerator>, kMovementSlotCount> m_slots;
    };

    // =========================================================================
    // MovementSystem
    // =========================================================================

    /**
     * @brief ECS system managing MotionControllers for all AI entities.
     *
     * Singleton. The AI layer calls MovementSystem::Update() each frame after
     * behavior tree ticking. It iterates all entities with AIComponent and
     * ticks their MotionControllers.
     */
    class MovementSystem
    {
      public:
        /** @brief Get the singleton instance. */
        static MovementSystem& GetInstance();

        void Initialize();
        void Update(World& world, float deltaTime);
        void Shutdown();

        // -- Per-entity movement commands --

        void MoveIdle(EntityID entity);
        void MoveChase(EntityID entity, EntityID target, float minDist = 0.5f, float maxDist = 2.0f);
        void MoveFollow(EntityID entity, EntityID target, float distance = 3.0f, float angle = 0.0f);
        void MoveFlee(EntityID entity, EntityID threat, float distance = 20.0f, float duration = 5.0f);
        void MovePoint(EntityID entity, float x, float y, float z, float speed = 5.0f);
        void MovePatrol(EntityID entity, const std::vector<std::array<float, 3>>& waypoints, bool loop = true);
        void MoveHome(EntityID entity, float x, float y, float z);
        void MoveRandomWander(EntityID entity, float radius = 10.0f, float waitTime = 3.0f);
        void MoveCharge(EntityID entity, EntityID target, float speed = 20.0f, float duration = 0.5f);
        void MoveSpline(EntityID entity, const std::vector<std::array<float, 3>>& controlPoints, float speed = 5.0f,
                        bool loop = false);
        void MoveWaypoint(EntityID entity, float x, float y, float z, float speed = 5.0f);

        void StopMovement(EntityID entity, MovementSlot slot);
        void StopAllMovement(EntityID entity);

        /** @brief Get the active movement type for an entity. */
        MovementType GetMovementType(EntityID entity) const;

      private:
        MovementSystem() = default;

        MotionController& GetOrCreateController(EntityID entity);

        std::unordered_map<EntityID, MotionController> m_controllers;
        bool m_initialized = false;
    };

} // namespace Spark::AI
