// TestDecalSystem.cpp - Tests for decal rendering system

#include "TestFramework.h"
#include "Graphics/DecalSystem.h"

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
    Decal d;
    d.opacity = 0.8f;
    d.active = true;
    // Opacity may be affected by fade logic, just ensure it's valid
    float opacity = d.GetCurrentOpacity();
    EXPECT_GE(opacity, 0.0f);
    EXPECT_LE(opacity, 1.0f);
}
