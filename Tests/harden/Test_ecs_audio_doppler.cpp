/**
 * @file Test_ecs_audio_doppler.cpp
 * @brief Regression test for the AudioUpdateSystem Doppler-velocity discontinuity guard.
 *
 * Bug (P1): AudioUpdateSystem derived source velocity as (position - previousPosition)/dt.
 * AudioSourceComponent::previousPosition defaults to {0,0,0}, so on an entity's first
 * update — or after any teleport (AI/spline snapping the Transform) — the derived speed
 * spikes to physically implausible values (e.g. spawn at (100,0,0), dt=0.016 -> 6250 m/s),
 * driving X3DAudio into an extreme/degenerate Doppler pitch shift.
 *
 * Fix: any derived speed beyond the speed of sound (~343 m/s) is treated as a first-frame
 * or teleport discontinuity and the velocity is left at zero instead of the spike.
 *
 * This test mirrors the exact guarded computation from ECSystems.cpp (standalone replica,
 * no EnTT/AudioEngine dependency — same CI-safe pattern as TestECSIntegration.cpp) and
 * asserts the invariant: normal sub-sonic motion yields the true velocity; first-frame and
 * teleport discontinuities yield zero.
 */

#include "TestFramework.h"

namespace
{
    struct Vec3
    {
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
    };

    /// Speed of sound (m/s) — must match kMaxDopplerSpeed in AudioUpdateSystem::Update.
    constexpr float kMaxDopplerSpeed = 343.0f;

    /// Mirror of the guarded Doppler-velocity computation in ECSystems.cpp.
    /// Returns {0,0,0} when the implied speed is implausible (first frame / teleport).
    Vec3 ComputeDopplerVelocity(const Vec3& current, const Vec3& previous, float deltaTime)
    {
        Vec3 velocity{};
        if (deltaTime > 1e-6f)
        {
            const float vx = (current.x - previous.x) / deltaTime;
            const float vy = (current.y - previous.y) / deltaTime;
            const float vz = (current.z - previous.z) / deltaTime;
            if (vx * vx + vy * vy + vz * vz <= kMaxDopplerSpeed * kMaxDopplerSpeed)
            {
                velocity = {vx, vy, vz};
            }
        }
        return velocity;
    }
} // namespace

TEST(EcsAudioDoppler_NormalSubsonicMotion_YieldsTrueVelocity)
{
    // Entity moves 1.6 m in +x over one 16 ms frame -> 100 m/s (well under Mach 1).
    const Vec3 previous{10.0f, 0.0f, 0.0f};
    const Vec3 current{11.6f, 0.0f, 0.0f};
    const Vec3 v = ComputeDopplerVelocity(current, previous, 0.016f);
    EXPECT_NEAR(v.x, 100.0f, 0.01f);
    EXPECT_NEAR(v.y, 0.0f, 1e-6f);
    EXPECT_NEAR(v.z, 0.0f, 1e-6f);
}

TEST(EcsAudioDoppler_FirstFrameSpawnAwayFromOrigin_YieldsZero)
{
    // First update: previousPosition is still the default {0,0,0} while the entity
    // spawned at (100,0,0). Naive formula -> 6250 m/s; guard must zero it.
    const Vec3 previous{0.0f, 0.0f, 0.0f};
    const Vec3 current{100.0f, 0.0f, 0.0f};
    const Vec3 v = ComputeDopplerVelocity(current, previous, 0.016f);
    EXPECT_NEAR(v.x, 0.0f, 1e-6f);
    EXPECT_NEAR(v.y, 0.0f, 1e-6f);
    EXPECT_NEAR(v.z, 0.0f, 1e-6f);
}

TEST(EcsAudioDoppler_Teleport_YieldsZero)
{
    // A one-frame teleport of 50 m -> 3125 m/s. Guard must suppress the spike.
    const Vec3 previous{5.0f, 1.0f, -2.0f};
    const Vec3 current{5.0f, 1.0f, 48.0f};
    const Vec3 v = ComputeDopplerVelocity(current, previous, 0.016f);
    EXPECT_NEAR(v.x, 0.0f, 1e-6f);
    EXPECT_NEAR(v.y, 0.0f, 1e-6f);
    EXPECT_NEAR(v.z, 0.0f, 1e-6f);
}

TEST(EcsAudioDoppler_JustBelowSpeedOfSound_Preserved)
{
    // 342 m/s is below the threshold and must be kept as a real Doppler velocity.
    const Vec3 previous{0.0f, 0.0f, 0.0f};
    const Vec3 current{342.0f * 0.016f, 0.0f, 0.0f};
    const Vec3 v = ComputeDopplerVelocity(current, previous, 0.016f);
    EXPECT_NEAR(v.x, 342.0f, 0.05f);
}
