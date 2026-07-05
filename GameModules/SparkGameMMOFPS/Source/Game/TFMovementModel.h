/**
 * @file TFMovementModel.h
 * @brief TF movement model v1 — THE shared client/server movement step.
 *
 * ============================ TF movement model v1 ==========================
 * Both the client prediction functor (TFClientNet) and the authoritative
 * server integrator (TFServerSim) MUST run this exact function with the same
 * inputs so reconciliation corrections stay near zero. Per-class max speeds
 * (runSpeed / sprintSpeed) come from classes.json (ClassDef); the constants
 * below are the frozen W1 movement contract:
 *
 *   accel        40.0 m/s^2        (kTFMoveAccel)
 *   friction      8.0 /s           (kTFFriction, ground only)
 *   gravity      19.6 m/s^2        (kTFGravity)
 *   jump          7.5 m/s          (kTFJumpSpeed)
 *   air control   0.3              (kTFAirControl, accel multiplier airborne)
 *
 * Step order (frozen — do not reorder, it changes the numbers):
 *   1. ground friction        vel.xz *= max(0, 1 - friction*dt)   [grounded]
 *   2. accelerate             vel.xz += wishDir * accel * control * dt
 *   3. clamp horizontal speed |vel.xz| <= maxSpeed
 *   4. jump                   if (jump && grounded) vel.y = jump; grounded=false
 *   5. gravity                vel.y -= gravity * dt
 *   6. integrate              pos += vel * dt
 *   7. terrain clamp          if pos.y <= h: pos.y = h, vel.y = max(vel.y, 0),
 *                             grounded = true; else grounded = (pos.y-h) < 0.05
 *
 * Basis (yaw in radians, camera convention — SparkEngineCamera::GetRotation):
 *   forward = ( sin(yaw), 0, cos(yaw) )
 *   right   = ( cos(yaw), 0, -sin(yaw) )
 *   wish    = forward*moveY + right*moveX   (normalized if |wish| > 1)
 * ===========================================================================
 */
#pragma once

#include <cmath>
#include <utility>

namespace Terrafront {

// Frozen movement constants (TF movement model v1)
constexpr float kTFMoveAccel   = 40.0f;   // m/s^2
constexpr float kTFFriction    = 8.0f;    // 1/s, ground only
constexpr float kTFGravity     = 19.6f;   // m/s^2
constexpr float kTFJumpSpeed   = 7.5f;    // m/s
constexpr float kTFAirControl  = 0.3f;    // accel multiplier while airborne
constexpr float kTFCrouchMult  = 0.5f;    // max-speed multiplier while crouched
constexpr float kTFGroundEps   = 0.05f;   // meters above terrain still "grounded"

// Pawn dimensions shared by camera, hitboxes, and lag-comp rewind poses.
constexpr float kTFEyeHeightM  = 1.65f;
constexpr float kTFPawnRadiusM = 0.4f;
constexpr float kTFPawnHeightM = 1.8f;

struct TFMoveState {
    float pos[3] = {0, 0, 0};
    float vel[3] = {0, 0, 0};
    bool  grounded = true;
};

struct TFMoveInput {
    float moveX = 0.0f;   ///< strafe  -1..1 (right positive)
    float moveY = 0.0f;   ///< forward -1..1
    float yaw   = 0.0f;   ///< radians
    bool  jump   = false;
    bool  sprint = false;
    bool  crouch = false;
};

/**
 * @brief Advance one movement tick (TF movement model v1 — see file header).
 *
 * @param s                In/out kinematic state.
 * @param in               This tick's input.
 * @param runSpeed         Class base max speed (classes.json runSpeed, m/s).
 * @param sprintSpeed      Class sprint max speed (classes.json sprintSpeed, m/s).
 * @param dt               Fixed tick delta (1/60 s on both sides).
 * @param terrainHeightAt  Callable float(float x, float z) -> ground height.
 */
template <typename TerrainFn>
inline void TFMoveStep(TFMoveState& s, const TFMoveInput& in, float runSpeed, float sprintSpeed,
                       float dt, TerrainFn&& terrainHeightAt)
{
    // wish direction in world XZ from view yaw
    const float sy = std::sin(in.yaw);
    const float cy = std::cos(in.yaw);
    float wishX = sy * in.moveY + cy * in.moveX;
    float wishZ = cy * in.moveY - sy * in.moveX;
    const float wishLen = std::sqrt(wishX * wishX + wishZ * wishZ);
    if (wishLen > 1.0f)
    {
        wishX /= wishLen;
        wishZ /= wishLen;
    }

    float maxSpeed = in.sprint ? sprintSpeed : runSpeed;
    if (in.crouch)
        maxSpeed *= kTFCrouchMult;

    // 1. ground friction
    if (s.grounded)
    {
        const float keep = 1.0f - kTFFriction * dt;
        const float k = keep > 0.0f ? keep : 0.0f;
        s.vel[0] *= k;
        s.vel[2] *= k;
    }

    // 2. accelerate
    const float control = s.grounded ? 1.0f : kTFAirControl;
    s.vel[0] += wishX * kTFMoveAccel * control * dt;
    s.vel[2] += wishZ * kTFMoveAccel * control * dt;

    // 3. clamp horizontal speed
    const float hs = std::sqrt(s.vel[0] * s.vel[0] + s.vel[2] * s.vel[2]);
    if (hs > maxSpeed && hs > 0.0f)
    {
        const float k = maxSpeed / hs;
        s.vel[0] *= k;
        s.vel[2] *= k;
    }

    // 4. jump
    if (in.jump && s.grounded)
    {
        s.vel[1] = kTFJumpSpeed;
        s.grounded = false;
    }

    // 5. gravity
    s.vel[1] -= kTFGravity * dt;

    // 6. integrate
    s.pos[0] += s.vel[0] * dt;
    s.pos[1] += s.vel[1] * dt;
    s.pos[2] += s.vel[2] * dt;

    // 7. terrain clamp
    const float h = terrainHeightAt(s.pos[0], s.pos[2]);
    if (s.pos[1] <= h)
    {
        s.pos[1] = h;
        if (s.vel[1] < 0.0f)
            s.vel[1] = 0.0f;
        s.grounded = true;
    }
    else
    {
        s.grounded = (s.pos[1] - h) < kTFGroundEps;
    }
}

} // namespace Terrafront
