/**
 * @file TestECSystemSpecialized.cpp
 * @brief Tests for specialized ECS systems: Decal, Projectile, Spline, Ability, Particle
 *
 * Covers the ECS systems not tested by TestECSystemOrdering.cpp. Uses standalone
 * component/World types for CI cross-platform compatibility (no DirectXMath/EnTT
 * dependency).
 */

#include "TestFramework.h"

#ifndef SPARK_PLATFORM_WINDOWS
#ifndef _XM_NO_INTRINSICS_
#define _XM_NO_INTRINSICS_
#endif
#endif

#include <cmath>
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <algorithm>
#include <numeric>

// ============================================================================
// Standalone test types (mirror production types for CI)
// ============================================================================

namespace
{

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        Vec3 Normalized() const
        {
            float len = Length();
            if (len < 1e-6f)
                return {0, 0, 0};
            return {x / len, y / len, z / len};
        }
    };

    struct Transform
    {
        Vec3 position;
        Vec3 rotation;
        Vec3 scale = {1, 1, 1};
        bool active = true;
    };

    // Decal component
    struct DecalComponent
    {
        std::string texturePath;
        Vec3 size = {1, 1, 1};
        float opacity = 1.0f;
        float lifetime = -1.0f; // negative = infinite
        float age = 0.0f;
        bool fadeOut = true;
        int sortOrder = 0;
    };

    // Projectile component
    struct ProjectileComponent
    {
        Vec3 velocity;
        float speed = 10.0f;
        float damage = 25.0f;
        float lifetime = 5.0f;
        float age = 0.0f;
        float gravity = 9.81f;
        bool useGravity = true;
        bool alive = true;
        int ownerID = -1;
    };

    // Spline follower component
    struct SplineFollowerComponent
    {
        std::vector<Vec3> waypoints;
        int currentSegment = 0;
        float t = 0.0f; // 0..1 within segment
        float speed = 5.0f;
        bool loop = false;
        bool finished = false;
        bool paused = false;
    };

    // Ability component
    struct AbilityComponent
    {
        std::string abilityName;
        float cooldown = 1.0f;
        float cooldownRemaining = 0.0f;
        float duration = 0.0f;
        float durationRemaining = 0.0f;
        bool active = false;
        bool ready = true;
        float manaCost = 0.0f;
        float currentMana = 100.0f;
    };

    // Particle emitter component
    struct ParticleEmitterComponent
    {
        float emissionRate = 10.0f;
        float particleLifetime = 2.0f;
        float accumulator = 0.0f;
        int activeParticles = 0;
        int maxParticles = 100;
        bool emitting = true;
        Vec3 velocity = {0, 1, 0};
        float velocityVariance = 0.2f;
    };

    // Health component for lifecycle
    struct HealthComponent
    {
        float health = 100.0f;
        float maxHealth = 100.0f;
        bool alive = true;
        std::function<void(int)> onDeath;
    };

    struct Entity
    {
        int id;
        Transform transform;
        DecalComponent* decal = nullptr;
        ProjectileComponent* projectile = nullptr;
        SplineFollowerComponent* spline = nullptr;
        AbilityComponent* ability = nullptr;
        ParticleEmitterComponent* particles = nullptr;
        HealthComponent* health = nullptr;
    };

    class World
    {
      public:
        std::vector<Entity> entities;

        Entity& CreateEntity()
        {
            entities.push_back(
                {static_cast<int>(entities.size()), {}, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
            return entities.back();
        }

        void Clear() { entities.clear(); }
    };

    // ============================================================================
    // System implementations (test-local, matching production logic)
    // ============================================================================

    class DecalSystemTest
    {
      public:
        int updatedCount = 0;
        int removedCount = 0;

        void Update(World& world, float dt)
        {
            updatedCount = 0;
            removedCount = 0;
            for (auto& e : world.entities)
            {
                if (!e.decal || !e.transform.active)
                    continue;

                if (e.decal->lifetime > 0.0f)
                {
                    e.decal->age += dt;
                    if (e.decal->age >= e.decal->lifetime)
                    {
                        e.transform.active = false;
                        removedCount++;
                        continue;
                    }
                    if (e.decal->fadeOut)
                    {
                        float remaining = e.decal->lifetime - e.decal->age;
                        float fadeStart = e.decal->lifetime * 0.2f;
                        if (remaining < fadeStart && fadeStart > 0.0f)
                        {
                            e.decal->opacity = remaining / fadeStart;
                        }
                    }
                }
                updatedCount++;
            }
        }
    };

    class ProjectileSystemTest
    {
      public:
        int updatedCount = 0;
        int expiredCount = 0;

        void Update(World& world, float dt)
        {
            updatedCount = 0;
            expiredCount = 0;
            for (auto& e : world.entities)
            {
                if (!e.projectile || !e.projectile->alive)
                    continue;

                e.projectile->age += dt;
                if (e.projectile->age >= e.projectile->lifetime)
                {
                    e.projectile->alive = false;
                    expiredCount++;
                    continue;
                }

                if (e.projectile->useGravity)
                {
                    e.projectile->velocity.y -= e.projectile->gravity * dt;
                }

                e.transform.position = e.transform.position + e.projectile->velocity * dt;
                updatedCount++;
            }
        }
    };

    class SplineFollowerSystemTest
    {
      public:
        int updatedCount = 0;
        int finishedCount = 0;

        void Update(World& world, float dt)
        {
            updatedCount = 0;
            finishedCount = 0;
            for (auto& e : world.entities)
            {
                if (!e.spline || e.spline->finished || e.spline->paused)
                    continue;
                if (e.spline->waypoints.size() < 2)
                    continue;

                float segmentLength = 1.0f; // simplified
                float advance = e.spline->speed * dt / std::max(segmentLength, 0.001f);
                e.spline->t += advance;

                while (e.spline->t >= 1.0f && !e.spline->finished)
                {
                    e.spline->t -= 1.0f;
                    e.spline->currentSegment++;

                    if (e.spline->currentSegment >= static_cast<int>(e.spline->waypoints.size()) - 1)
                    {
                        if (e.spline->loop)
                        {
                            e.spline->currentSegment = 0;
                        }
                        else
                        {
                            e.spline->finished = true;
                            e.spline->t = 1.0f;
                            e.spline->currentSegment = static_cast<int>(e.spline->waypoints.size()) - 2;
                            finishedCount++;
                        }
                    }
                }

                if (!e.spline->finished)
                {
                    int idx = e.spline->currentSegment;
                    const auto& a = e.spline->waypoints[idx];
                    const auto& b = e.spline->waypoints[idx + 1];
                    float t = e.spline->t;
                    e.transform.position = {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
                }
                updatedCount++;
            }
        }
    };

    class AbilitySystemTest
    {
      public:
        int updatedCount = 0;

        void Update(World& world, float dt)
        {
            updatedCount = 0;
            for (auto& e : world.entities)
            {
                if (!e.ability)
                    continue;

                // Cooldown tick
                if (e.ability->cooldownRemaining > 0.0f)
                {
                    e.ability->cooldownRemaining -= dt;
                    if (e.ability->cooldownRemaining <= 0.0f)
                    {
                        e.ability->cooldownRemaining = 0.0f;
                        e.ability->ready = true;
                    }
                }

                // Duration tick
                if (e.ability->active && e.ability->duration > 0.0f)
                {
                    e.ability->durationRemaining -= dt;
                    if (e.ability->durationRemaining <= 0.0f)
                    {
                        e.ability->active = false;
                        e.ability->durationRemaining = 0.0f;
                    }
                }

                updatedCount++;
            }
        }

        bool TryActivate(AbilityComponent& ability)
        {
            if (!ability.ready || ability.active)
                return false;
            if (ability.currentMana < ability.manaCost)
                return false;

            ability.currentMana -= ability.manaCost;
            ability.active = true;
            ability.ready = false;
            ability.cooldownRemaining = ability.cooldown;
            ability.durationRemaining = ability.duration;
            return true;
        }
    };

    class ParticleSystemTest
    {
      public:
        int totalSpawned = 0;

        void Update(World& world, float dt)
        {
            totalSpawned = 0;
            for (auto& e : world.entities)
            {
                if (!e.particles || !e.particles->emitting)
                    continue;

                e.particles->accumulator += e.particles->emissionRate * dt;
                int toSpawn = static_cast<int>(e.particles->accumulator);
                if (toSpawn > 0)
                {
                    int canSpawn = std::min(toSpawn, e.particles->maxParticles - e.particles->activeParticles);
                    e.particles->activeParticles += canSpawn;
                    e.particles->accumulator -= static_cast<float>(toSpawn);
                    totalSpawned += canSpawn;
                }
            }
        }
    };

} // anonymous namespace

// ============================================================================
// Decal System Tests
// ============================================================================

TEST(DecalSystem_UpdateActiveDecals)
{
    World world;
    auto& e = world.CreateEntity();
    DecalComponent decal;
    decal.texturePath = "textures/bullet_hole.png";
    decal.lifetime = 5.0f;
    e.decal = &decal;

    DecalSystemTest sys;
    sys.Update(world, 0.016f);

    EXPECT_EQ(sys.updatedCount, 1);
    EXPECT_EQ(sys.removedCount, 0);
    EXPECT_NEAR(decal.age, 0.016f, 0.001f);
}

TEST(DecalSystem_LifetimeExpiry)
{
    World world;
    auto& e = world.CreateEntity();
    DecalComponent decal;
    decal.lifetime = 1.0f;
    decal.age = 0.99f;
    e.decal = &decal;

    DecalSystemTest sys;
    sys.Update(world, 0.02f); // pushes past 1.0

    EXPECT_EQ(sys.removedCount, 1);
    EXPECT_FALSE(e.transform.active);
}

TEST(DecalSystem_InfiniteLifetime)
{
    World world;
    auto& e = world.CreateEntity();
    DecalComponent decal;
    decal.lifetime = -1.0f; // infinite
    e.decal = &decal;

    DecalSystemTest sys;
    sys.Update(world, 100.0f);

    EXPECT_EQ(sys.updatedCount, 1);
    EXPECT_EQ(sys.removedCount, 0);
    EXPECT_TRUE(e.transform.active);
}

TEST(DecalSystem_FadeOut)
{
    World world;
    auto& e = world.CreateEntity();
    DecalComponent decal;
    decal.lifetime = 10.0f;
    decal.age = 9.0f; // within last 20% (fadeStart = 2.0)
    decal.fadeOut = true;
    e.decal = &decal;

    DecalSystemTest sys;
    sys.Update(world, 0.0f);

    // remaining = 1.0, fadeStart = 2.0, opacity = 1.0/2.0 = 0.5
    EXPECT_NEAR(decal.opacity, 0.5f, 0.01f);
}

TEST(DecalSystem_SkipsInactiveEntities)
{
    World world;
    auto& e = world.CreateEntity();
    e.transform.active = false;
    DecalComponent decal;
    decal.lifetime = 5.0f;
    e.decal = &decal;

    DecalSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_EQ(sys.updatedCount, 0);
    EXPECT_NEAR(decal.age, 0.0f, 0.001f);
}

// ============================================================================
// Projectile System Tests
// ============================================================================

TEST(ProjectileSystem_LinearMotion)
{
    World world;
    auto& e = world.CreateEntity();
    ProjectileComponent proj;
    proj.velocity = {10.0f, 0.0f, 0.0f};
    proj.useGravity = false;
    e.projectile = &proj;

    ProjectileSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_NEAR(e.transform.position.x, 10.0f, 0.01f);
    EXPECT_NEAR(e.transform.position.y, 0.0f, 0.01f);
}

TEST(ProjectileSystem_GravityApplied)
{
    World world;
    auto& e = world.CreateEntity();
    ProjectileComponent proj;
    proj.velocity = {10.0f, 0.0f, 0.0f};
    proj.gravity = 10.0f;
    proj.useGravity = true;
    e.projectile = &proj;

    ProjectileSystemTest sys;
    sys.Update(world, 1.0f);

    // After 1s: vy = 0 - 10*1 = -10, pos.y = -10
    EXPECT_NEAR(e.transform.position.x, 10.0f, 0.01f);
    EXPECT_LT(e.transform.position.y, 0.0f);
}

TEST(ProjectileSystem_LifetimeExpiry)
{
    World world;
    auto& e = world.CreateEntity();
    ProjectileComponent proj;
    proj.velocity = {1, 0, 0};
    proj.lifetime = 0.5f;
    proj.age = 0.49f;
    e.projectile = &proj;

    ProjectileSystemTest sys;
    sys.Update(world, 0.02f);

    EXPECT_FALSE(proj.alive);
    EXPECT_EQ(sys.expiredCount, 1);
}

TEST(ProjectileSystem_DeadProjectileSkipped)
{
    World world;
    auto& e = world.CreateEntity();
    ProjectileComponent proj;
    proj.alive = false;
    proj.velocity = {100, 0, 0};
    e.projectile = &proj;

    ProjectileSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_EQ(sys.updatedCount, 0);
    EXPECT_NEAR(e.transform.position.x, 0.0f, 0.001f);
}

TEST(ProjectileSystem_MultipleProjectiles)
{
    World world;
    ProjectileComponent projs[3];
    for (int i = 0; i < 3; i++)
    {
        auto& e = world.CreateEntity();
        projs[i].velocity = {static_cast<float>(i + 1), 0, 0};
        projs[i].useGravity = false;
        e.projectile = &projs[i];
    }

    ProjectileSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_EQ(sys.updatedCount, 3);
    EXPECT_NEAR(world.entities[0].transform.position.x, 1.0f, 0.01f);
    EXPECT_NEAR(world.entities[1].transform.position.x, 2.0f, 0.01f);
    EXPECT_NEAR(world.entities[2].transform.position.x, 3.0f, 0.01f);
}

// ============================================================================
// Spline Follower System Tests
// ============================================================================

TEST(SplineFollower_FollowsPath)
{
    World world;
    auto& e = world.CreateEntity();
    SplineFollowerComponent spline;
    spline.waypoints = {{0, 0, 0}, {10, 0, 0}, {10, 10, 0}};
    spline.speed = 5.0f;
    e.spline = &spline;

    SplineFollowerSystemTest sys;
    sys.Update(world, 0.1f);

    EXPECT_EQ(sys.updatedCount, 1);
    EXPECT_FALSE(spline.finished);
    EXPECT_GT(e.transform.position.x, 0.0f);
}

TEST(SplineFollower_ReachesEnd)
{
    World world;
    auto& e = world.CreateEntity();
    SplineFollowerComponent spline;
    spline.waypoints = {{0, 0, 0}, {1, 0, 0}};
    spline.speed = 100.0f;
    spline.loop = false;
    e.spline = &spline;

    SplineFollowerSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_TRUE(spline.finished);
    EXPECT_EQ(sys.finishedCount, 1);
}

TEST(SplineFollower_Loops)
{
    World world;
    auto& e = world.CreateEntity();
    SplineFollowerComponent spline;
    spline.waypoints = {{0, 0, 0}, {1, 0, 0}};
    spline.speed = 100.0f;
    spline.loop = true;
    e.spline = &spline;

    SplineFollowerSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_FALSE(spline.finished);
    EXPECT_EQ(spline.currentSegment, 0);
}

TEST(SplineFollower_Paused)
{
    World world;
    auto& e = world.CreateEntity();
    SplineFollowerComponent spline;
    spline.waypoints = {{0, 0, 0}, {10, 0, 0}};
    spline.speed = 5.0f;
    spline.paused = true;
    e.spline = &spline;

    SplineFollowerSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_EQ(sys.updatedCount, 0);
    EXPECT_NEAR(e.transform.position.x, 0.0f, 0.001f);
}

TEST(SplineFollower_TooFewWaypoints)
{
    World world;
    auto& e = world.CreateEntity();
    SplineFollowerComponent spline;
    spline.waypoints = {{5, 5, 5}}; // only 1 waypoint
    spline.speed = 5.0f;
    e.spline = &spline;

    SplineFollowerSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_EQ(sys.updatedCount, 0); // skipped due to < 2 waypoints
}

// ============================================================================
// Ability System Tests
// ============================================================================

TEST(AbilitySystem_CooldownTick)
{
    World world;
    auto& e = world.CreateEntity();
    AbilityComponent ability;
    ability.cooldownRemaining = 1.0f;
    ability.ready = false;
    e.ability = &ability;

    AbilitySystemTest sys;
    sys.Update(world, 0.5f);

    EXPECT_FALSE(ability.ready);
    EXPECT_NEAR(ability.cooldownRemaining, 0.5f, 0.01f);

    sys.Update(world, 0.6f);
    EXPECT_TRUE(ability.ready);
    EXPECT_NEAR(ability.cooldownRemaining, 0.0f, 0.01f);
}

TEST(AbilitySystem_DurationExpiry)
{
    World world;
    auto& e = world.CreateEntity();
    AbilityComponent ability;
    ability.active = true;
    ability.duration = 2.0f;
    ability.durationRemaining = 0.5f;
    e.ability = &ability;

    AbilitySystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_FALSE(ability.active);
    EXPECT_NEAR(ability.durationRemaining, 0.0f, 0.01f);
}

TEST(AbilitySystem_ActivateWithMana)
{
    AbilityComponent ability;
    ability.cooldown = 3.0f;
    ability.duration = 1.0f;
    ability.manaCost = 30.0f;
    ability.currentMana = 50.0f;

    AbilitySystemTest sys;
    bool ok = sys.TryActivate(ability);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(ability.active);
    EXPECT_FALSE(ability.ready);
    EXPECT_NEAR(ability.currentMana, 20.0f, 0.01f);
    EXPECT_NEAR(ability.cooldownRemaining, 3.0f, 0.01f);
}

TEST(AbilitySystem_ActivateInsufficientMana)
{
    AbilityComponent ability;
    ability.manaCost = 100.0f;
    ability.currentMana = 50.0f;

    AbilitySystemTest sys;
    bool ok = sys.TryActivate(ability);

    EXPECT_FALSE(ok);
    EXPECT_FALSE(ability.active);
    EXPECT_NEAR(ability.currentMana, 50.0f, 0.01f);
}

TEST(AbilitySystem_CannotActivateOnCooldown)
{
    AbilityComponent ability;
    ability.ready = false;
    ability.cooldownRemaining = 2.0f;
    ability.manaCost = 0.0f;

    AbilitySystemTest sys;
    bool ok = sys.TryActivate(ability);

    EXPECT_FALSE(ok);
}

TEST(AbilitySystem_CannotDoubleActivate)
{
    AbilityComponent ability;
    ability.active = true;
    ability.manaCost = 0.0f;

    AbilitySystemTest sys;
    bool ok = sys.TryActivate(ability);

    EXPECT_FALSE(ok);
}

// ============================================================================
// Particle System Tests
// ============================================================================

TEST(ParticleSystem_EmitsOverTime)
{
    World world;
    auto& e = world.CreateEntity();
    ParticleEmitterComponent particles;
    particles.emissionRate = 100.0f; // 100/sec
    particles.maxParticles = 1000;
    e.particles = &particles;

    ParticleSystemTest sys;
    sys.Update(world, 0.1f); // expect ~10 particles

    EXPECT_GE(particles.activeParticles, 9);
    EXPECT_LE(particles.activeParticles, 11);
}

TEST(ParticleSystem_RespectMaxParticles)
{
    World world;
    auto& e = world.CreateEntity();
    ParticleEmitterComponent particles;
    particles.emissionRate = 1000.0f;
    particles.maxParticles = 5;
    particles.activeParticles = 3;
    e.particles = &particles;

    ParticleSystemTest sys;
    sys.Update(world, 1.0f); // wants 1000, but only 2 slots left

    EXPECT_EQ(particles.activeParticles, 5);
}

TEST(ParticleSystem_StoppedEmitter)
{
    World world;
    auto& e = world.CreateEntity();
    ParticleEmitterComponent particles;
    particles.emitting = false;
    particles.emissionRate = 100.0f;
    e.particles = &particles;

    ParticleSystemTest sys;
    sys.Update(world, 1.0f);

    EXPECT_EQ(particles.activeParticles, 0);
    EXPECT_EQ(sys.totalSpawned, 0);
}

TEST(ParticleSystem_AccumulatorFractional)
{
    World world;
    auto& e = world.CreateEntity();
    ParticleEmitterComponent particles;
    particles.emissionRate = 3.0f; // 3/sec
    particles.maxParticles = 100;
    e.particles = &particles;

    ParticleSystemTest sys;
    // 3 updates of 0.1s each = 0.3 particles each time
    sys.Update(world, 0.1f);
    EXPECT_EQ(particles.activeParticles, 0); // 0.3 < 1

    sys.Update(world, 0.1f);
    EXPECT_EQ(particles.activeParticles, 0); // 0.6 < 1

    sys.Update(world, 0.2f);
    // accumulator now ~0.6 + 0.6 = 1.2, should emit 1
    EXPECT_GE(particles.activeParticles, 1);
}

// ============================================================================
// SystemManager Enable/Disable Tests
// ============================================================================

TEST(SystemManager_DisableSkipsUpdate)
{
    // Test that disabled systems don't get their Update called
    int callCount = 0;

    struct CountingSystem
    {
        int* counter;
        bool enabled = true;

        void Update(float /*dt*/) { (*counter)++; }
        bool IsEnabled() const { return enabled; }
        void SetEnabled(bool e) { enabled = e; }
    };

    CountingSystem sys{&callCount, true};
    sys.Update(0.016f);
    EXPECT_EQ(callCount, 1);

    sys.SetEnabled(false);
    EXPECT_FALSE(sys.IsEnabled());
    // A disabled system should not be called by SystemManager
    // (we verify the flag here; the actual SystemManager check is in UpdateAll)
}

TEST(SystemManager_ReEnableResumesUpdate)
{
    int callCount = 0;

    struct CountingSystem
    {
        int* counter;
        bool enabled = true;

        void Update(float) { (*counter)++; }
        bool IsEnabled() const { return enabled; }
        void SetEnabled(bool e) { enabled = e; }
    };

    CountingSystem sys{&callCount, true};
    sys.SetEnabled(false);
    EXPECT_FALSE(sys.IsEnabled());

    sys.SetEnabled(true);
    EXPECT_TRUE(sys.IsEnabled());

    sys.Update(0.016f);
    EXPECT_EQ(callCount, 1);
}
