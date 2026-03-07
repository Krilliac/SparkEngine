/**
 * @file SteeringBehaviors.h
 * @brief Steering behavior utilities for AI agent locomotion
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a collection of classic steering behaviors for autonomous agents:
 * - Individual behaviors: Seek, Flee, Arrive, Pursue, Evade, Wander
 * - Obstacle avoidance
 * - Flocking behaviors: Separation, Alignment, Cohesion
 *
 * All functions are header-only inline implementations operating on
 * DirectXMath XMFLOAT3 vectors. Each returns a desired velocity vector
 * that can be combined and applied to an agent's transform.
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

#include <vector>
#include <cmath>
#include <random>


namespace Spark::AI {

// ============================================================================
// Obstacle representation for avoidance
// ============================================================================

/// A simple sphere obstacle used by ObstacleAvoidance
struct Obstacle {
    XMFLOAT3 position;
    float radius = 1.0f;
};

// ============================================================================
// Internal helpers
// ============================================================================

namespace SteeringDetail {

/// Compute the squared length of a 3D vector
inline float LengthSq(const XMFLOAT3& v) {
    return v.x * v.x + v.y * v.y + v.z * v.z;
}

/// Compute the length of a 3D vector
inline float Length(const XMFLOAT3& v) {
    return std::sqrt(LengthSq(v));
}

/// Normalize a 3D vector; returns zero vector if length is near zero
inline XMFLOAT3 Normalize(const XMFLOAT3& v) {
    float len = Length(v);
    if (len < 1e-6f) return XMFLOAT3(0.0f, 0.0f, 0.0f);
    float inv = 1.0f / len;
    return XMFLOAT3(v.x * inv, v.y * inv, v.z * inv);
}

/// Subtract two 3D vectors: a - b
inline XMFLOAT3 Sub(const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.x - b.x, a.y - b.y, a.z - b.z);
}

/// Add two 3D vectors: a + b
inline XMFLOAT3 Add(const XMFLOAT3& a, const XMFLOAT3& b) {
    return XMFLOAT3(a.x + b.x, a.y + b.y, a.z + b.z);
}

/// Scale a 3D vector by a scalar
inline XMFLOAT3 Scale(const XMFLOAT3& v, float s) {
    return XMFLOAT3(v.x * s, v.y * s, v.z * s);
}

/// Dot product of two 3D vectors
inline float Dot(const XMFLOAT3& a, const XMFLOAT3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

/// Truncate a vector to a maximum magnitude
inline XMFLOAT3 Truncate(const XMFLOAT3& v, float maxMagnitude) {
    float lenSq = LengthSq(v);
    if (lenSq > maxMagnitude * maxMagnitude) {
        float inv = maxMagnitude / std::sqrt(lenSq);
        return XMFLOAT3(v.x * inv, v.y * inv, v.z * inv);
    }
    return v;
}

} // namespace SteeringDetail

// ============================================================================
// Steering Behaviors
// ============================================================================

namespace SteeringBehaviors {

/// Seek: steer toward a target position at maximum speed.
///
/// Returns a velocity vector pointing from the agent's current position
/// directly toward the target, with magnitude equal to maxSpeed.
///
/// @param position  Current position of the agent
/// @param target    Position to seek toward
/// @param maxSpeed  Maximum speed of the agent
/// @return Desired velocity vector
inline XMFLOAT3 Seek(const XMFLOAT3& position, const XMFLOAT3& target, float maxSpeed) {
    XMFLOAT3 desired = SteeringDetail::Sub(target, position);
    desired = SteeringDetail::Normalize(desired);
    return SteeringDetail::Scale(desired, maxSpeed);
}

/// Flee: steer away from a threat position at maximum speed.
///
/// The inverse of Seek. Returns a velocity vector pointing directly away
/// from the threat, with magnitude equal to maxSpeed.
///
/// @param position  Current position of the agent
/// @param threat    Position to flee from
/// @param maxSpeed  Maximum speed of the agent
/// @return Desired velocity vector
inline XMFLOAT3 Flee(const XMFLOAT3& position, const XMFLOAT3& threat, float maxSpeed) {
    XMFLOAT3 desired = SteeringDetail::Sub(position, threat);
    desired = SteeringDetail::Normalize(desired);
    return SteeringDetail::Scale(desired, maxSpeed);
}

/// Arrive: seek a target but decelerate as the agent approaches.
///
/// When the agent is farther than slowRadius from the target, it moves at
/// maxSpeed. Inside the slow radius, speed is linearly ramped down to zero,
/// producing a smooth stop at the target.
///
/// @param position    Current position of the agent
/// @param target      Position to arrive at
/// @param maxSpeed    Maximum speed of the agent
/// @param slowRadius  Distance at which the agent begins to decelerate
/// @return Desired velocity vector with deceleration applied
inline XMFLOAT3 Arrive(const XMFLOAT3& position, const XMFLOAT3& target,
                        float maxSpeed, float slowRadius) {
    XMFLOAT3 toTarget = SteeringDetail::Sub(target, position);
    float distance = SteeringDetail::Length(toTarget);

    if (distance < 1e-6f) {
        return XMFLOAT3(0.0f, 0.0f, 0.0f);
    }

    // Ramp speed based on distance within the slow radius
    float speed = maxSpeed;
    if (distance < slowRadius) {
        speed = maxSpeed * (distance / slowRadius);
    }

    XMFLOAT3 desired = SteeringDetail::Normalize(toTarget);
    return SteeringDetail::Scale(desired, speed);
}

/// Pursue: intercept a moving target by predicting its future position.
///
/// Estimates where the target will be based on its current velocity and
/// the distance between agent and target, then seeks that predicted point.
/// The look-ahead time is proportional to the distance divided by the
/// combined speeds of both entities.
///
/// @param position        Current position of the agent
/// @param targetPos       Current position of the target
/// @param targetVelocity  Current velocity of the target
/// @param maxSpeed        Maximum speed of the agent
/// @return Desired velocity vector toward the predicted intercept point
inline XMFLOAT3 Pursue(const XMFLOAT3& position, const XMFLOAT3& targetPos,
                        const XMFLOAT3& targetVelocity, float maxSpeed) {
    XMFLOAT3 toTarget = SteeringDetail::Sub(targetPos, position);
    float distance = SteeringDetail::Length(toTarget);

    // Estimate look-ahead time based on distance and relative speeds
    float targetSpeed = SteeringDetail::Length(targetVelocity);
    float lookAheadTime = distance / (maxSpeed + targetSpeed + 1e-6f);

    // Predict where the target will be
    XMFLOAT3 predictedPos = SteeringDetail::Add(
        targetPos, SteeringDetail::Scale(targetVelocity, lookAheadTime));

    return Seek(position, predictedPos, maxSpeed);
}

/// Evade: flee from a moving threat by predicting its future position.
///
/// The counterpart to Pursue. Estimates where the threat will be and
/// steers away from that predicted position.
///
/// @param position         Current position of the agent
/// @param threatPos        Current position of the threat
/// @param threatVelocity   Current velocity of the threat
/// @param maxSpeed         Maximum speed of the agent
/// @return Desired velocity vector away from the predicted threat position
inline XMFLOAT3 Evade(const XMFLOAT3& position, const XMFLOAT3& threatPos,
                       const XMFLOAT3& threatVelocity, float maxSpeed) {
    XMFLOAT3 toThreat = SteeringDetail::Sub(threatPos, position);
    float distance = SteeringDetail::Length(toThreat);

    float threatSpeed = SteeringDetail::Length(threatVelocity);
    float lookAheadTime = distance / (maxSpeed + threatSpeed + 1e-6f);

    XMFLOAT3 predictedPos = SteeringDetail::Add(
        threatPos, SteeringDetail::Scale(threatVelocity, lookAheadTime));

    return Flee(position, predictedPos, maxSpeed);
}

/// ObstacleAvoidance: adjust velocity to steer around nearby obstacles.
///
/// Projects a detection feeler along the agent's current velocity direction.
/// If the feeler intersects any obstacle's bounding sphere (expanded by
/// avoidRadius), a lateral avoidance force is applied proportional to the
/// proximity of the closest obstacle.
///
/// @param position     Current position of the agent
/// @param velocity     Current velocity of the agent
/// @param obstacles    List of sphere obstacles to avoid
/// @param avoidRadius  Extra clearance radius around the agent
/// @return Adjusted velocity that steers around the nearest obstacle
inline XMFLOAT3 ObstacleAvoidance(const XMFLOAT3& position, const XMFLOAT3& velocity,
                                   const std::vector<Obstacle>& obstacles,
                                   float avoidRadius) {
    float speed = SteeringDetail::Length(velocity);
    if (speed < 1e-6f) return velocity;

    XMFLOAT3 forward = SteeringDetail::Normalize(velocity);

    // Detection feeler length scales with speed
    float feelerLength = avoidRadius + speed;

    // Find the closest intersecting obstacle along the feeler
    float closestDist = feelerLength + 1.0f;
    int closestIdx = -1;
    XMFLOAT3 closestLocalOffset(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < static_cast<int>(obstacles.size()); ++i) {
        XMFLOAT3 toObs = SteeringDetail::Sub(obstacles[i].position, position);

        // Project obstacle center onto the forward axis
        float forwardProj = SteeringDetail::Dot(toObs, forward);

        // Skip obstacles behind the agent or beyond the feeler
        if (forwardProj < 0.0f || forwardProj > feelerLength) continue;

        // Compute lateral distance from the feeler line
        XMFLOAT3 projPoint = SteeringDetail::Add(
            position, SteeringDetail::Scale(forward, forwardProj));
        XMFLOAT3 lateralVec = SteeringDetail::Sub(obstacles[i].position, projPoint);
        float lateralDist = SteeringDetail::Length(lateralVec);

        float expandedRadius = obstacles[i].radius + avoidRadius;
        if (lateralDist < expandedRadius) {
            if (forwardProj < closestDist) {
                closestDist = forwardProj;
                closestIdx = i;
                closestLocalOffset = lateralVec;
            }
        }
    }

    // No intersection found -- return original velocity
    if (closestIdx < 0) return velocity;

    // Compute avoidance steering force
    // Steer laterally away from the obstacle, inversely proportional to distance
    float lateralLen = SteeringDetail::Length(closestLocalOffset);
    XMFLOAT3 avoidDir;
    if (lateralLen < 1e-6f) {
        // Agent is heading straight at the obstacle center -- pick an arbitrary
        // perpendicular direction using cross product with the up vector
        avoidDir = XMFLOAT3(-forward.z, 0.0f, forward.x);
    } else {
        // Steer away from the obstacle center
        avoidDir = SteeringDetail::Scale(closestLocalOffset, -1.0f / lateralLen);
    }

    // Stronger avoidance when closer
    float urgency = 1.0f - (closestDist / feelerLength);
    XMFLOAT3 avoidForce = SteeringDetail::Scale(avoidDir, speed * urgency);

    // Also apply a braking force along the forward direction
    float brakingWeight = 0.2f * urgency;
    XMFLOAT3 braking = SteeringDetail::Scale(forward, -speed * brakingWeight);

    return SteeringDetail::Add(SteeringDetail::Add(velocity, avoidForce), braking);
}

/// Wander: produce a random yet smoothly varying exploratory velocity.
///
/// Conceptually projects a circle of wanderRadius at wanderDistance in front
/// of the agent, then jitters a target point on that circle. The jitter is
/// applied each frame to produce smooth, natural-looking exploration without
/// sudden directional changes.
///
/// The caller must maintain the random engine between calls so that the
/// wander behavior evolves over time.
///
/// @param position        Current position of the agent
/// @param velocity        Current velocity of the agent
/// @param wanderRadius    Radius of the wander circle
/// @param wanderDistance  Distance of the wander circle center ahead of the agent
/// @param wanderJitter    Maximum random displacement per call
/// @param rng             Random engine (modified in place)
/// @return Desired velocity for random exploration
inline XMFLOAT3 Wander(const XMFLOAT3& position, const XMFLOAT3& velocity,
                        float wanderRadius, float wanderDistance, float wanderJitter,
                        std::mt19937& rng) {
    (void)position; // position not needed directly; we work relative to velocity

    float speed = SteeringDetail::Length(velocity);
    if (speed < 1e-6f) {
        // Agent is stationary -- pick a random direction
        std::uniform_real_distribution<float> angleDist(0.0f, XM_2PI);
        float angle = angleDist(rng);
        return XMFLOAT3(std::cos(angle) * wanderRadius,
                        0.0f,
                        std::sin(angle) * wanderRadius);
    }

    XMFLOAT3 forward = SteeringDetail::Normalize(velocity);

    // Compute a right vector (perpendicular on the XZ plane)
    // Using cross product of forward with world up (0,1,0)
    XMFLOAT3 right(forward.z, 0.0f, -forward.x);
    float rightLen = SteeringDetail::Length(right);
    if (rightLen < 1e-6f) {
        // Forward is nearly vertical; use world X as fallback
        right = XMFLOAT3(1.0f, 0.0f, 0.0f);
    } else {
        right = SteeringDetail::Normalize(right);
    }

    // Random jitter on the wander circle
    std::uniform_real_distribution<float> jitterDist(-1.0f, 1.0f);
    float jitterForward = jitterDist(rng) * wanderJitter;
    float jitterRight = jitterDist(rng) * wanderJitter;

    // Wander target on the circle ahead of the agent
    XMFLOAT3 circleCenter = SteeringDetail::Scale(forward, wanderDistance);
    XMFLOAT3 displacement = SteeringDetail::Add(
        SteeringDetail::Scale(forward, jitterForward),
        SteeringDetail::Scale(right, jitterRight));
    displacement = SteeringDetail::Normalize(displacement);
    displacement = SteeringDetail::Scale(displacement, wanderRadius);

    XMFLOAT3 wanderTarget = SteeringDetail::Add(circleCenter, displacement);
    wanderTarget = SteeringDetail::Normalize(wanderTarget);
    return SteeringDetail::Scale(wanderTarget, speed);
}

/// Separation: steer away from nearby neighbors to avoid crowding.
///
/// Part of the Reynolds flocking model. Produces a repulsive force that
/// pushes the agent away from each neighbor within the separation radius,
/// weighted inversely by distance so that closer neighbors exert a
/// stronger force.
///
/// @param position          Current position of the agent
/// @param neighbors         Positions of neighboring agents
/// @param separationRadius  Distance within which neighbors exert influence
/// @return Steering force away from nearby neighbors
inline XMFLOAT3 Separation(const XMFLOAT3& position,
                            const std::vector<XMFLOAT3>& neighbors,
                            float separationRadius) {
    XMFLOAT3 force(0.0f, 0.0f, 0.0f);
    int count = 0;

    for (const auto& neighbor : neighbors) {
        XMFLOAT3 toAgent = SteeringDetail::Sub(position, neighbor);
        float dist = SteeringDetail::Length(toAgent);

        if (dist < 1e-6f || dist > separationRadius) continue;

        // Weight inversely by distance: closer neighbors push harder
        XMFLOAT3 normalized = SteeringDetail::Normalize(toAgent);
        force = SteeringDetail::Add(force,
            SteeringDetail::Scale(normalized, 1.0f / dist));
        count++;
    }

    if (count > 0) {
        force = SteeringDetail::Scale(force, 1.0f / static_cast<float>(count));
    }

    return force;
}

/// Alignment: steer toward the average heading of nearby neighbors.
///
/// Part of the Reynolds flocking model. Returns the average velocity
/// direction of the neighbors, causing the agent to match the flock's
/// general heading.
///
/// @param velocity            Current velocity of the agent
/// @param neighborVelocities  Velocities of neighboring agents
/// @return Desired velocity aligned with neighbor headings
inline XMFLOAT3 Alignment(const XMFLOAT3& velocity,
                           const std::vector<XMFLOAT3>& neighborVelocities) {
    if (neighborVelocities.empty()) return velocity;

    XMFLOAT3 avgVelocity(0.0f, 0.0f, 0.0f);
    for (const auto& nv : neighborVelocities) {
        avgVelocity = SteeringDetail::Add(avgVelocity, nv);
    }

    float invCount = 1.0f / static_cast<float>(neighborVelocities.size());
    avgVelocity = SteeringDetail::Scale(avgVelocity, invCount);

    // Return the difference between average heading and current velocity
    // This acts as a steering force toward alignment
    return SteeringDetail::Sub(avgVelocity, velocity);
}

/// Cohesion: steer toward the average position of nearby neighbors.
///
/// Part of the Reynolds flocking model. Computes the centroid of
/// neighboring positions and returns a vector toward it, pulling the
/// agent toward the center of the local flock.
///
/// @param position           Current position of the agent
/// @param neighborPositions  Positions of neighboring agents
/// @return Steering force toward the flock centroid
inline XMFLOAT3 Cohesion(const XMFLOAT3& position,
                          const std::vector<XMFLOAT3>& neighborPositions) {
    if (neighborPositions.empty()) return XMFLOAT3(0.0f, 0.0f, 0.0f);

    XMFLOAT3 centroid(0.0f, 0.0f, 0.0f);
    for (const auto& np : neighborPositions) {
        centroid = SteeringDetail::Add(centroid, np);
    }

    float invCount = 1.0f / static_cast<float>(neighborPositions.size());
    centroid = SteeringDetail::Scale(centroid, invCount);

    // Return the direction toward the centroid (unnormalized so magnitude
    // encodes how far the agent is from the flock center)
    return SteeringDetail::Sub(centroid, position);
}

} // namespace SteeringBehaviors

} // namespace Spark::AI
