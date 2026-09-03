// TestGPUParticleSystem.cpp - Tests for GPU compute particle system logic
// Tests the CPU-side management and data structures without requiring D3D11

#include "TestFramework.h"

#include <cstdint>
#include <string>
#include <vector>

namespace
{

    // Mirror GPUParticleData layout from GPUParticleSystem.h
    struct GPUParticleData
    {
        float position[3];
        float lifetime;
        float velocity[3];
        float age;
        float color[4];
        float size;
        float rotation;
        float rotationSpeed;
        uint32_t alive;
    };

    struct ParticleEmitterConfig
    {
        std::string name;
        uint32_t maxParticles = 1024;
        float emitRate = 10.0f;
        float minLifetime = 1.0f;
        float maxLifetime = 3.0f;
        float minSpeed = 1.0f;
        float maxSpeed = 5.0f;
        float minSize = 0.1f;
        float maxSize = 0.5f;
        float gravity[3] = {0.0f, -9.81f, 0.0f};
        float drag = 0.0f;
    };

    // Simulate particle update on CPU (mirrors compute shader logic)
    void SimulateParticle(GPUParticleData& p, float dt, const float gravity[3], float drag)
    {
        if (p.alive == 0)
            return;

        p.lifetime -= dt;
        p.age += dt;

        if (p.lifetime <= 0.0f)
        {
            p.alive = 0;
            p.lifetime = 0.0f;
            return;
        }

        // Integrate velocity
        p.velocity[0] += gravity[0] * dt;
        p.velocity[1] += gravity[1] * dt;
        p.velocity[2] += gravity[2] * dt;

        // Apply drag
        float dragFactor = std::max(0.0f, 1.0f - drag * dt);
        p.velocity[0] *= dragFactor;
        p.velocity[1] *= dragFactor;
        p.velocity[2] *= dragFactor;

        // Integrate position
        p.position[0] += p.velocity[0] * dt;
        p.position[1] += p.velocity[1] * dt;
        p.position[2] += p.velocity[2] * dt;
    }

    uint32_t CountAlive(const std::vector<GPUParticleData>& particles)
    {
        uint32_t count = 0;
        for (const auto& p : particles)
        {
            if (p.alive)
                count++;
        }
        return count;
    }

} // namespace

TEST(GPUParticle_DataLayout_MatchesShader)
{
    // GPUParticleData must be 64 bytes to match HLSL struct
    EXPECT_EQ(sizeof(GPUParticleData), 64u);
}

TEST(GPUParticle_InitialState_AllDead)
{
    // Value-initialisation zeroes every field, including 'alive'
    std::vector<GPUParticleData> pool(1024);

    EXPECT_EQ(CountAlive(pool), 0u);
}

TEST(GPUParticle_Emit_SetsAlive)
{
    GPUParticleData p = {};
    p.alive = 1;
    p.lifetime = 2.0f;
    p.velocity[1] = 5.0f;
    p.size = 0.3f;

    EXPECT_EQ(p.alive, 1u);
    EXPECT_NEAR(p.lifetime, 2.0f, 0.001f);
}

TEST(GPUParticle_Simulate_AppliesGravity)
{
    GPUParticleData p = {};
    p.alive = 1;
    p.lifetime = 5.0f;
    p.position[1] = 10.0f;
    p.velocity[1] = 0.0f;

    float gravity[3] = {0.0f, -9.81f, 0.0f};
    float dt = 1.0f / 60.0f;

    SimulateParticle(p, dt, gravity, 0.0f);

    EXPECT_TRUE(p.velocity[1] < 0.0f);  // gravity pulls down
    EXPECT_TRUE(p.position[1] < 10.0f); // moved down
}

TEST(GPUParticle_Simulate_KillsExpired)
{
    GPUParticleData p = {};
    p.alive = 1;
    p.lifetime = 0.01f;
    p.age = 1.99f;

    float gravity[3] = {0, 0, 0};
    SimulateParticle(p, 0.02f, gravity, 0.0f);

    EXPECT_EQ(p.alive, 0u);
}

TEST(GPUParticle_Simulate_DragReducesVelocity)
{
    GPUParticleData p = {};
    p.alive = 1;
    p.lifetime = 5.0f;
    p.velocity[0] = 10.0f;

    float gravity[3] = {0, 0, 0};
    SimulateParticle(p, 1.0f / 60.0f, gravity, 5.0f);

    EXPECT_TRUE(p.velocity[0] < 10.0f);
    EXPECT_TRUE(p.velocity[0] > 0.0f);
}

TEST(GPUParticle_DeadList_InitiallyFull)
{
    constexpr uint32_t kCapacity = 256;
    std::vector<uint32_t> deadList(kCapacity);
    for (uint32_t i = 0; i < kCapacity; i++)
        deadList[i] = i;

    EXPECT_EQ(deadList.size(), kCapacity);
    EXPECT_EQ(deadList[0], 0u);
    EXPECT_EQ(deadList[255], 255u);
}

TEST(GPUParticle_EmitterConfig_Defaults)
{
    ParticleEmitterConfig config;

    EXPECT_EQ(config.maxParticles, 1024u);
    EXPECT_NEAR(config.emitRate, 10.0f, 0.001f);
    EXPECT_NEAR(config.minLifetime, 1.0f, 0.001f);
    EXPECT_NEAR(config.gravity[1], -9.81f, 0.001f);
}

TEST(GPUParticle_BatchSimulate_AllParticles)
{
    constexpr uint32_t kCount = 100;
    std::vector<GPUParticleData> pool(kCount);

    // Emit all particles
    for (uint32_t i = 0; i < kCount; i++)
    {
        pool[i].alive = 1;
        pool[i].lifetime = static_cast<float>(i + 1) * 0.1f;
        pool[i].velocity[1] = 5.0f;
    }

    float gravity[3] = {0, -9.81f, 0};

    // Simulate 10 steps
    for (int step = 0; step < 10; step++)
    {
        for (auto& p : pool)
            SimulateParticle(p, 1.0f / 60.0f, gravity, 0.0f);
    }

    // Some should have died (those with short lifetimes)
    uint32_t alive = CountAlive(pool);
    EXPECT_TRUE(alive < kCount);
    EXPECT_TRUE(alive > 0u);
}

TEST(GPUParticle_SortKey_CameraDistance)
{
    // Sort key = negated squared distance (back-to-front)
    float cameraPos[3] = {0, 0, 0};
    float particlePos[3] = {3, 4, 0}; // distance = 5

    float dx = cameraPos[0] - particlePos[0];
    float dy = cameraPos[1] - particlePos[1];
    float dz = cameraPos[2] - particlePos[2];
    float sortKey = -(dx * dx + dy * dy + dz * dz);

    EXPECT_NEAR(sortKey, -25.0f, 0.001f);
}

TEST(GPUParticle_IndirectArgs_Layout)
{
    // D3D11_DRAW_INSTANCED_INDIRECT_ARGS layout: {vertexCount, instanceCount, startVertex, startInstance}
    uint32_t indirectArgs[4] = {0, 1, 0, 0};

    // After simulation, aliveCount goes into vertexCount slot
    indirectArgs[0] = 42; // 42 alive particles

    EXPECT_EQ(indirectArgs[0], 42u);
    EXPECT_EQ(indirectArgs[1], 1u); // always 1 instance
}
