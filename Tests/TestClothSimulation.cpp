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
