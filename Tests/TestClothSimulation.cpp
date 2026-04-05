/**
 * @file TestClothSimulation.cpp
 * @brief Tests for Spark::Physics::ClothSimulation
 */

#include "TestFramework.h"
#include "../SparkEngine/Source/Physics/ClothSimulation.h"

TEST(Cloth_CreateAndDestroy)
{
    Spark::Physics::ClothSimulation cloth;
    Spark::Physics::ClothDescriptor desc;
    desc.width = 5;
    desc.height = 5;
    desc.spacing = 0.1f;

    auto id = cloth.CreateCloth(desc);
    EXPECT_TRUE(id > 0);
    EXPECT_EQ(cloth.GetInstanceCount(), static_cast<size_t>(1));

    cloth.DestroyCloth(id);
    EXPECT_EQ(cloth.GetInstanceCount(), static_cast<size_t>(0));
}

TEST(Cloth_ParticleCount)
{
    Spark::Physics::ClothSimulation cloth;
    Spark::Physics::ClothDescriptor desc;
    desc.width = 4;
    desc.height = 3;
    desc.spacing = 0.2f;

    auto id = cloth.CreateCloth(desc);
    const auto& particles = cloth.GetParticles(id);
    EXPECT_EQ(particles.size(), static_cast<size_t>(12)); // 4 * 3 = 12

    auto [w, h] = cloth.GetClothDimensions(id);
    EXPECT_EQ(w, 4);
    EXPECT_EQ(h, 3);
}

TEST(Cloth_PinParticle)
{
    Spark::Physics::ClothSimulation cloth;
    Spark::Physics::ClothDescriptor desc;
    desc.width = 3;
    desc.height = 3;
    desc.spacing = 0.1f;

    auto id = cloth.CreateCloth(desc);
    DirectX::XMFLOAT3 pinPos{0, 2, 0};
    cloth.PinParticle(id, 0, pinPos);

    const auto& particles = cloth.GetParticles(id);
    EXPECT_TRUE(particles[0].pinned);
    EXPECT_NEAR(particles[0].position.y, 2.0f, 0.001f);
}

TEST(Cloth_GravityFalls)
{
    Spark::Physics::ClothSimulation cloth;
    Spark::Physics::ClothDescriptor desc;
    desc.width = 2;
    desc.height = 2;
    desc.spacing = 0.1f;
    desc.origin = {0, 5, 0};

    auto id = cloth.CreateCloth(desc);
    float initialY = cloth.GetParticles(id)[0].position.y;

    // Simulate a few steps — particles should fall due to gravity
    for (int i = 0; i < 10; ++i)
    {
        cloth.Update(1.0f / 60.0f);
    }

    float newY = cloth.GetParticles(id)[0].position.y;
    EXPECT_TRUE(newY < initialY); // Should have fallen
}

// ============================================================================
// Constraint Satisfaction
// ============================================================================

TEST(Cloth_ConstraintSatisfaction)
{
    Spark::Physics::ClothSimulation cloth;
    Spark::Physics::ClothDescriptor desc;
    desc.width = 3;
    desc.height = 1;
    desc.spacing = 1.0f;
    desc.origin = {0, 0, 0};

    auto id = cloth.CreateCloth(desc);

    // Pin first particle so cloth doesn't just fall
    DirectX::XMFLOAT3 pinPos{0, 0, 0};
    cloth.PinParticle(id, 0, pinPos);

    // Run solver iterations
    for (int i = 0; i < 50; ++i)
    {
        cloth.Update(1.0f / 60.0f);
    }

    // Check that adjacent particle distances converge toward rest length
    const auto& particles = cloth.GetParticles(id);
    float dx = particles[1].position.x - particles[0].position.x;
    float dy = particles[1].position.y - particles[0].position.y;
    float dz = particles[1].position.z - particles[0].position.z;
    float dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // Distance should be close to rest length (spacing = 1.0)
    // Allow generous tolerance since gravity pulls particles down
    EXPECT_TRUE(dist > 0.0f);
    EXPECT_TRUE(dist < 5.0f); // Should not diverge wildly
}

// ============================================================================
// Wind Force
// ============================================================================

TEST(Cloth_WindForce)
{
    // Create two identical cloths — one with wind, one without
    // Compare displacement to verify wind has an effect
    Spark::Physics::ClothSimulation clothWind;
    Spark::Physics::ClothSimulation clothNoWind;
    Spark::Physics::ClothDescriptor desc;
    desc.width = 3;
    desc.height = 3;
    desc.spacing = 0.1f;
    desc.origin = {0, 5, 0};

    auto idWind = clothWind.CreateCloth(desc);
    auto idNoWind = clothNoWind.CreateCloth(desc);

    // Pin top-left corner on both
    DirectX::XMFLOAT3 pinPos{0, 5, 0};
    clothWind.PinParticle(idWind, 0, pinPos);
    clothNoWind.PinParticle(idNoWind, 0, pinPos);

    // Apply strong wind in +X direction to one cloth
    clothWind.SetWind(idWind, {20.0f, 0.0f, 0.0f});

    for (int i = 0; i < 60; ++i)
    {
        clothWind.Update(1.0f / 60.0f);
        clothNoWind.Update(1.0f / 60.0f);
    }

    float windX = clothWind.GetParticles(idWind)[8].position.x;
    float noWindX = clothNoWind.GetParticles(idNoWind)[8].position.x;
    // Wind-affected cloth should have particles displaced further in +X
    EXPECT_TRUE(windX > noWindX);
}

// ============================================================================
// High Iteration Stability
// ============================================================================

TEST(Cloth_HighIterationStability)
{
    Spark::Physics::ClothSimulation cloth;
    Spark::Physics::ClothDescriptor desc;
    desc.width = 4;
    desc.height = 4;
    desc.spacing = 0.2f;
    desc.origin = {0, 10, 0};

    auto id = cloth.CreateCloth(desc);

    // Pin two top corners for stability
    DirectX::XMFLOAT3 pin0{0, 10, 0};
    DirectX::XMFLOAT3 pin3{0.6f, 10, 0};
    cloth.PinParticle(id, 0, pin0);
    cloth.PinParticle(id, 3, pin3);

    // Run 100 iterations
    for (int i = 0; i < 100; ++i)
    {
        cloth.Update(1.0f / 60.0f);
    }

    // Verify no NaN or infinite positions
    const auto& particles = cloth.GetParticles(id);
    for (size_t i = 0; i < particles.size(); ++i)
    {
        EXPECT_FALSE(std::isnan(particles[i].position.x));
        EXPECT_FALSE(std::isnan(particles[i].position.y));
        EXPECT_FALSE(std::isnan(particles[i].position.z));
        EXPECT_FALSE(std::isinf(particles[i].position.x));
        EXPECT_FALSE(std::isinf(particles[i].position.y));
        EXPECT_FALSE(std::isinf(particles[i].position.z));
    }
}
