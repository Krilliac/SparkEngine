/**
 * @file TestSpatialGridReal.cpp
 * @brief Real-class tests for Spark::World::SpatialGrid
 *
 * Phase JJ deep-wire.
 */

#include "TestFramework.h"
#include "Engine/World/SpatialGrid.h"

TEST(SpatialGridReal_DefaultConstruction)
{
    Spark::World::SpatialGrid grid;
    // Default cellSize 64.0. Verify WorldToCell origin maps to (0, 0).
    const auto cell = grid.WorldToCell(0.0f, 0.0f);
    EXPECT_EQ(cell.x, 0);
    EXPECT_EQ(cell.z, 0);
}

TEST(SpatialGridReal_WorldToCellBoundaries)
{
    Spark::World::SpatialGrid grid(10.0f); // 10-unit cells
    EXPECT_EQ(grid.WorldToCell(5.0f, 5.0f).x, 0);
    EXPECT_EQ(grid.WorldToCell(5.0f, 5.0f).z, 0);
    EXPECT_EQ(grid.WorldToCell(10.0f, 10.0f).x, 1);
    EXPECT_EQ(grid.WorldToCell(25.0f, 35.0f).x, 2);
    EXPECT_EQ(grid.WorldToCell(25.0f, 35.0f).z, 3);
}

TEST(SpatialGridReal_WorldToCellNegative)
{
    Spark::World::SpatialGrid grid(10.0f);
    EXPECT_EQ(grid.WorldToCell(-5.0f, -5.0f).x, -1);
    EXPECT_EQ(grid.WorldToCell(-5.0f, -5.0f).z, -1);
}

TEST(SpatialGridReal_AddAndRemoveEntity)
{
    Spark::World::SpatialGrid grid(10.0f);
    auto entity = static_cast<entt::entity>(42);

    grid.AddEntity(entity, 15.0f, 25.0f);
    const auto cell = grid.WorldToCell(15.0f, 25.0f);
    const auto* c = grid.GetCell(cell);
    EXPECT_TRUE(c != nullptr);
    if (c)
    {
        EXPECT_EQ(c->GetEntityCount(), 1);
    }

    grid.RemoveEntity(entity);
    const auto* c2 = grid.GetCell(cell);
    if (c2)
    {
        EXPECT_EQ(c2->GetEntityCount(), 0);
    }
}

TEST(SpatialGridReal_MoveEntityBetweenCells)
{
    Spark::World::SpatialGrid grid(10.0f);
    auto entity = static_cast<entt::entity>(7);

    grid.AddEntity(entity, 5.0f, 5.0f);
    grid.MoveEntity(entity, 25.0f, 25.0f);

    const auto newCell = grid.WorldToCell(25.0f, 25.0f);
    const auto* c = grid.GetCell(newCell);
    EXPECT_TRUE(c != nullptr);
    if (c)
    {
        EXPECT_EQ(c->GetEntityCount(), 1);
    }
}

TEST(SpatialGridReal_GetEntitiesInRadius)
{
    Spark::World::SpatialGrid grid(10.0f);
    grid.AddEntity(static_cast<entt::entity>(1), 0.0f, 0.0f);
    grid.AddEntity(static_cast<entt::entity>(2), 5.0f, 5.0f);
    grid.AddEntity(static_cast<entt::entity>(3), 100.0f, 100.0f); // Far away.

    auto nearbyEntities = grid.GetEntitiesInRadius(0.0f, 0.0f, 15.0f);
    EXPECT_TRUE(nearbyEntities.size() >= static_cast<size_t>(2));
}

TEST(SpatialGridReal_CellCoordEquality)
{
    Spark::World::CellCoord a{1, 2};
    Spark::World::CellCoord b{1, 2};
    Spark::World::CellCoord c{3, 4};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}
