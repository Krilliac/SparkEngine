// TestHLODSystem.cpp - Tests for Spark::HLOD::HLODSystem
#include "TestFramework.h"
#include "Engine/HLOD/HLODSystem.h"

TEST(HLODSystem_Initialize)
{
    auto& sys = Spark::HLOD::HLODSystem::GetInstance();
    sys.Initialize();
    EXPECT_NEAR(1000.0f, sys.GetLoadRadius(), 0.1f);
    EXPECT_NEAR(1500.0f, sys.GetUnloadRadius(), 0.1f);
    sys.Shutdown();
}

TEST(HLODSystem_GridSetup)
{
    auto& sys = Spark::HLOD::HLODSystem::GetInstance();
    sys.Initialize();
    Spark::HLOD::WorldBounds bounds{{0, 0, 0}, {1024, 256, 1024}};
    sys.GetGrid().Initialize(bounds, 256.0f);
    EXPECT_NEAR(256.0f, sys.GetGrid().GetCellSize(), 0.01f);
    sys.Shutdown();
}

TEST(HLODSystem_CellAssignment)
{
    auto& sys = Spark::HLOD::HLODSystem::GetInstance();
    sys.Initialize();
    Spark::HLOD::WorldBounds bounds{{0, 0, 0}, {1024, 256, 1024}};
    sys.GetGrid().Initialize(bounds, 256.0f);
    sys.GetGrid().AssignEntity(1, {100.0f, 0.0f, 100.0f});
    sys.GetGrid().AssignEntity(2, {100.0f, 0.0f, 150.0f});
    sys.GetGrid().AssignEntity(3, {500.0f, 0.0f, 500.0f});
    const auto* cell = sys.GetGrid().GetCellAt({100.0f, 0.0f, 100.0f});
    EXPECT_TRUE(cell != nullptr);
    EXPECT_EQ(static_cast<size_t>(2), cell->entities.size());
    sys.Shutdown();
}

TEST(HLODSystem_RemoveEntity)
{
    auto& sys = Spark::HLOD::HLODSystem::GetInstance();
    sys.Initialize();
    Spark::HLOD::WorldBounds bounds{{0, 0, 0}, {512, 128, 512}};
    sys.GetGrid().Initialize(bounds, 256.0f);
    sys.GetGrid().AssignEntity(10, {50.0f, 0.0f, 50.0f});
    sys.GetGrid().AssignEntity(20, {50.0f, 0.0f, 50.0f});
    sys.GetGrid().RemoveEntity(10);
    const auto* cell = sys.GetGrid().GetCellAt({50.0f, 0.0f, 50.0f});
    EXPECT_TRUE(cell != nullptr);
    EXPECT_EQ(static_cast<size_t>(1), cell->entities.size());
    sys.Shutdown();
}

TEST(HLODSystem_StreamingLoadUnload)
{
    auto& sys = Spark::HLOD::HLODSystem::GetInstance();
    sys.Initialize();
    sys.SetLoadRadius(300.0f);
    sys.SetUnloadRadius(500.0f);
    Spark::HLOD::WorldBounds bounds{{0, 0, 0}, {2048, 256, 2048}};
    sys.GetGrid().Initialize(bounds, 256.0f);
    sys.GetGrid().AssignEntity(1, {128.0f, 0.0f, 128.0f});
    sys.GetGrid().AssignEntity(2, {1800.0f, 0.0f, 1800.0f});
    sys.Update({100.0f, 0.0f, 100.0f});
    EXPECT_TRUE(sys.GetGrid().GetLoadedCells().size() >= 1);
    const auto* farCell = sys.GetGrid().GetCellAt({1800.0f, 0.0f, 1800.0f});
    if (farCell)
        EXPECT_TRUE(farCell->loadState == Spark::HLOD::CellLoadState::Unloaded);
    sys.Shutdown();
}

TEST(HLODSystem_StreamingUnloadDistant)
{
    auto& sys = Spark::HLOD::HLODSystem::GetInstance();
    sys.Initialize();
    sys.SetLoadRadius(500.0f);
    sys.SetUnloadRadius(700.0f);
    Spark::HLOD::WorldBounds bounds{{0, 0, 0}, {2048, 256, 2048}};
    sys.GetGrid().Initialize(bounds, 256.0f);
    sys.GetGrid().AssignEntity(1, {128.0f, 0.0f, 128.0f});
    sys.Update({128.0f, 0.0f, 128.0f});
    EXPECT_TRUE(sys.GetGrid().GetLoadedCells().size() >= 1);
    sys.Update({1900.0f, 0.0f, 1900.0f});
    const auto* cell = sys.GetGrid().GetCellAt({128.0f, 0.0f, 128.0f});
    if (cell)
        EXPECT_TRUE(cell->loadState == Spark::HLOD::CellLoadState::Unloaded);
    sys.Shutdown();
}

TEST(HLODSystem_ClusterBuilding)
{
    Spark::HLOD::HLODBuilder builder;
    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 128.0f;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities = {
        {1, {10.0f, 0.0f, 10.0f}, 2.0f, 1000},
        {2, {20.0f, 0.0f, 15.0f}, 2.0f, 800},
        {3, {500.0f, 0.0f, 500.0f}, 2.0f, 600},
    };
    builder.BuildClusters(entities, settings);
    EXPECT_TRUE(builder.GetClusters().size() >= 2);
    bool foundPair = false;
    for (const auto& c : builder.GetClusters())
        if (c.sourceEntities.size() == 2)
            foundPair = true;
    EXPECT_TRUE(foundPair);
}

TEST(HLODSystem_ProxyGeneration)
{
    Spark::HLOD::HLODBuilder builder;
    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 256.0f;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities = {
        {1, {10.0f, 0.0f, 10.0f}, 2.0f, 10000},
    };
    builder.BuildClusters(entities, settings);
    EXPECT_FALSE(builder.GetClusters().empty());
    auto proxy = builder.GenerateProxy(builder.GetClusters()[0], 5000);
    EXPECT_TRUE(proxy.lodLevel == Spark::HLOD::HLODLevel::LOD1);
    EXPECT_EQ(static_cast<uint32_t>(5000), proxy.vertexCount);
    auto imposter = builder.GenerateImposter(builder.GetClusters()[0]);
    EXPECT_TRUE(imposter.lodLevel == Spark::HLOD::HLODLevel::LOD3);
    EXPECT_EQ(static_cast<uint32_t>(4), imposter.vertexCount);
}

TEST(HLODSystem_VisibleClusters)
{
    auto& sys = Spark::HLOD::HLODSystem::GetInstance();
    sys.Initialize();
    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 256.0f;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities = {
        {1, {0.0f, 0.0f, 0.0f}, 2.0f, 100},
        {2, {5000.0f, 0.0f, 5000.0f}, 2.0f, 100},
    };
    sys.GetBuilder().BuildClusters(entities, settings);
    sys.BuildHLOD(settings);
    auto visible = sys.GetVisibleClusters({0.0f, 0.0f, 0.0f}, 500.0f);
    EXPECT_EQ(static_cast<size_t>(1), visible.size());
    sys.Shutdown();
}
