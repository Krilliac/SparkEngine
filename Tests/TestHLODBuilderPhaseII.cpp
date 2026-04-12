/**
 * @file TestHLODBuilderPhaseII.cpp
 * @brief Phase II Theme 3D tests for Spark::HLOD::HLODBuilder
 *
 * HLODBuilder is a build-time tool that groups entities into HLOD
 * clusters based on spatial grid and generates proxy meshes per LOD
 * level. Per-instance class — no singleton wire-up needed.
 */

#include "TestFramework.h"
#include "Engine/HLOD/HLODSystem.h"

#include <vector>

namespace
{
    Spark::HLOD::HLODBuilder::EntityEntry MakeEntity(uint32_t id, float x, float y, float z, float radius = 1.0f,
                                                     uint32_t tris = 1000)
    {
        Spark::HLOD::HLODBuilder::EntityEntry e;
        e.entityId = id;
        e.position = {x, y, z};
        e.boundingRadius = radius;
        e.triangleCount = tris;
        return e;
    }
} // namespace

TEST(HLODBuilderPhaseII_DefaultConstructs)
{
    Spark::HLOD::HLODBuilder builder;
    EXPECT_EQ(builder.GetClusters().size(), static_cast<size_t>(0));
    EXPECT_EQ(builder.GetProxies().size(), static_cast<size_t>(0));
}

TEST(HLODBuilderPhaseII_BuildClustersFromSingleEntity)
{
    Spark::HLOD::HLODBuilder builder;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities;
    entities.push_back(MakeEntity(1, 0, 0, 0));

    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 128.0f;
    builder.BuildClusters(entities, settings);

    EXPECT_EQ(builder.GetClusters().size(), static_cast<size_t>(1));
}

TEST(HLODBuilderPhaseII_BuildClustersGroupsSameCell)
{
    Spark::HLOD::HLODBuilder builder;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities;
    // Three entities in the same 128x128 cell.
    entities.push_back(MakeEntity(1, 10, 0, 10));
    entities.push_back(MakeEntity(2, 20, 0, 20));
    entities.push_back(MakeEntity(3, 30, 0, 30));

    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 128.0f;
    builder.BuildClusters(entities, settings);

    EXPECT_EQ(builder.GetClusters().size(), static_cast<size_t>(1));
    if (!builder.GetClusters().empty())
    {
        EXPECT_EQ(builder.GetClusters()[0].sourceEntities.size(), static_cast<size_t>(3));
    }
}

TEST(HLODBuilderPhaseII_BuildClustersSplitsAcrossCells)
{
    Spark::HLOD::HLODBuilder builder;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities;
    // Two entities far apart — 1000 units spacing exceeds 128 cell.
    entities.push_back(MakeEntity(1, 0, 0, 0));
    entities.push_back(MakeEntity(2, 1000, 0, 1000));

    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 128.0f;
    builder.BuildClusters(entities, settings);

    EXPECT_EQ(builder.GetClusters().size(), static_cast<size_t>(2));
}

TEST(HLODBuilderPhaseII_ClusterBoundsContainSources)
{
    Spark::HLOD::HLODBuilder builder;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities;
    entities.push_back(MakeEntity(1, 10, 0, 10, /*radius*/ 5.0f));
    entities.push_back(MakeEntity(2, 50, 0, 50, /*radius*/ 5.0f));

    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 128.0f;
    builder.BuildClusters(entities, settings);

    EXPECT_EQ(builder.GetClusters().size(), static_cast<size_t>(1));
    if (!builder.GetClusters().empty())
    {
        const auto& c = builder.GetClusters()[0];
        // Bounds should encompass both entities including radii.
        EXPECT_TRUE(c.boundsMin.x <= 5.0f);  // 10 - 5
        EXPECT_TRUE(c.boundsMax.x >= 55.0f); // 50 + 5
        EXPECT_TRUE(c.radius > 0.0f);
    }
}

TEST(HLODBuilderPhaseII_GenerateProxyProducesResult)
{
    Spark::HLOD::HLODBuilder builder;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities;
    entities.push_back(MakeEntity(1, 0, 0, 0, 1.0f, 10000));

    Spark::HLOD::HLODBuildSettings settings;
    settings.clusterSize = 128.0f;
    settings.lod1TargetTriangles = 5000;
    settings.lod2TargetTriangles = 500;
    builder.BuildClusters(entities, settings);

    const auto& clusters = builder.GetClusters();
    EXPECT_EQ(clusters.size(), static_cast<size_t>(1));
    if (!clusters.empty())
    {
        auto proxy = builder.GenerateProxy(clusters[0], 5000);
        EXPECT_TRUE(proxy.vertexCount > 0);
        EXPECT_TRUE(proxy.indexCount > 0);
    }
}

TEST(HLODBuilderPhaseII_GenerateImposterProducesResult)
{
    Spark::HLOD::HLODBuilder builder;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities;
    entities.push_back(MakeEntity(1, 0, 0, 0));

    Spark::HLOD::HLODBuildSettings settings;
    builder.BuildClusters(entities, settings);

    const auto& clusters = builder.GetClusters();
    EXPECT_EQ(clusters.size(), static_cast<size_t>(1));
    if (!clusters.empty())
    {
        auto imposter = builder.GenerateImposter(clusters[0]);
        // Imposter is a quad — 4 verts, 6 indices.
        EXPECT_EQ(imposter.vertexCount, static_cast<uint32_t>(4));
        EXPECT_EQ(imposter.indexCount, static_cast<uint32_t>(6));
    }
}

TEST(HLODBuilderPhaseII_EmptyInputProducesNoClusters)
{
    Spark::HLOD::HLODBuilder builder;
    std::vector<Spark::HLOD::HLODBuilder::EntityEntry> entities;
    Spark::HLOD::HLODBuildSettings settings;
    builder.BuildClusters(entities, settings);
    EXPECT_EQ(builder.GetClusters().size(), static_cast<size_t>(0));
}
