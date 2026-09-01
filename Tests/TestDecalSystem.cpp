// TestDecalSystem.cpp - Tests for decal rendering system

#include "TestFramework.h"
#include "Graphics/DecalSystem.h"
#include <limits>

using namespace Spark::Graphics;

TEST(DecalSystem_InitAndShutdown)
{
    auto& ds = DecalSystem::GetInstance();
    ds.Initialize(256);
    EXPECT_EQ(ds.GetActiveDecalCount(), static_cast<uint32_t>(0));
    ds.Shutdown();
}

TEST(DecalSystem_RegisterMaterial)
{
    auto& ds = DecalSystem::GetInstance();
    ds.Initialize(128);

    DecalMaterial mat;
    mat.name = "BulletHole";
    mat.opacity = 0.9f;
    ds.RegisterMaterial(mat);

    auto* found = ds.GetMaterial("BulletHole");
    EXPECT_NE(found, nullptr);
    EXPECT_EQ(found->name, std::string("BulletHole"));

    EXPECT_EQ(ds.GetMaterial("NonExistent"), nullptr);
    ds.Shutdown();
}

TEST(DecalSystem_SpawnDecal)
{
    auto& ds = DecalSystem::GetInstance();
    ds.Initialize(128);

    auto* decal =
        ds.SpawnDecal({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, DecalType::BulletHole, SurfaceType::Concrete, 0.1f);
    EXPECT_NE(decal, nullptr);
    EXPECT_EQ(ds.GetActiveDecalCount(), static_cast<uint32_t>(1));

    ds.Shutdown();
}

TEST(DecalSystem_ClearAllDecals)
{
    auto& ds = DecalSystem::GetInstance();
    ds.Initialize(128);

    ds.SpawnDecal({0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, DecalType::BulletHole);
    ds.SpawnDecal({1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, DecalType::ScorchMark);
    EXPECT_EQ(ds.GetActiveDecalCount(), static_cast<uint32_t>(2));

    ds.ClearAllDecals();
    EXPECT_EQ(ds.GetActiveDecalCount(), static_cast<uint32_t>(0));

    ds.Shutdown();
}

TEST(DecalSystem_SetMaxDecals)
{
    auto& ds = DecalSystem::GetInstance();
    ds.Initialize(64);
    EXPECT_NO_THROW(ds.SetMaxDecals(128));
    ds.Shutdown();
}

TEST(DecalSystem_ConsoleStatus)
{
    auto& ds = DecalSystem::GetInstance();
    ds.Initialize(64);
    std::string status = ds.Console_GetStatus();
    EXPECT_FALSE(status.empty());
    ds.Shutdown();
}

TEST(DecalSystem_DecalGetCurrentOpacity)
{
    Decal d{};
    d.opacity = 0.8f;
    d.active = true;
    d.fadeTimer = 2.0f;
    d.fadeDuration = 4.0f;

    d.age = 1.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.8f);
    d.age = 2.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.8f);
    d.age = 4.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.4f);
    d.age = 6.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.0f);

    d.active = false;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.0f);

    d.active = true;
    d.age = 0.0f;
    d.fadeTimer = 0.0f;
    d.fadeDuration = 0.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.0f);
    d.age = 1.0f;
    d.fadeDuration = -1.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.0f);
    d.fadeDuration = std::numeric_limits<float>::infinity();
    EXPECT_EQ(d.GetCurrentOpacity(), 0.0f);

    d.fadeTimer = 2.0f;
    d.fadeDuration = 4.0f;
    d.age = 1.0f;
    d.opacity = 2.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 1.0f);
    d.opacity = -1.0f;
    EXPECT_EQ(d.GetCurrentOpacity(), 0.0f);
    d.opacity = 0.8f;
    d.age = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(d.GetCurrentOpacity(), 0.0f);
}
