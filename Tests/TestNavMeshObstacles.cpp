// TestNavMeshObstacles.cpp - Tests for dynamic navmesh obstacle management

#include "TestFramework.h"
#include "Engine/AI/NavMeshObstacles.h"

using namespace Spark::AI;

TEST(NavObstacles_InitiallyEmpty)
{
    auto& mgr = NavMeshObstacleManager::GetInstance();
    mgr.Clear();
    EXPECT_EQ(mgr.GetObstacleCount(), static_cast<size_t>(0));
}

TEST(NavObstacles_AddObstacle)
{
    auto& mgr = NavMeshObstacleManager::GetInstance();
    mgr.Clear();

    ObstacleDesc desc;
    desc.shape = ObstacleShape::Box;
    desc.position = {10.0f, 0.0f, 10.0f};
    desc.halfExtents = {1.0f, 1.0f, 1.0f};

    // Without a navmesh, AddObstacle still returns a handle but may not fully track
    EXPECT_NO_THROW(mgr.AddObstacle(desc));
    mgr.Clear();
}

TEST(NavObstacles_RemoveObstacle)
{
    auto& mgr = NavMeshObstacleManager::GetInstance();
    mgr.Clear();

    ObstacleDesc desc;
    desc.shape = ObstacleShape::Cylinder;
    desc.position = {5.0f, 0.0f, 5.0f};
    desc.radius = 2.0f;
    desc.height = 3.0f;

    ObstacleHandle h = mgr.AddObstacle(desc);
    // Remove should not crash regardless of navmesh state
    EXPECT_NO_THROW(mgr.RemoveObstacle(h));
    mgr.Clear();
}

TEST(NavObstacles_RemoveInvalidHandle)
{
    auto& mgr = NavMeshObstacleManager::GetInstance();
    mgr.Clear();
    bool removed = mgr.RemoveObstacle(InvalidObstacleHandle);
    EXPECT_FALSE(removed);
}

TEST(NavObstacles_GetObstacleDesc)
{
    auto& mgr = NavMeshObstacleManager::GetInstance();
    mgr.Clear();

    // Without navmesh, GetObstacleDesc for invalid handle returns false
    ObstacleDesc retrieved;
    EXPECT_FALSE(mgr.GetObstacleDesc(InvalidObstacleHandle, retrieved));
    mgr.Clear();
}

TEST(NavObstacles_UpdateObstacle)
{
    auto& mgr = NavMeshObstacleManager::GetInstance();
    mgr.Clear();

    ObstacleDesc desc;
    desc.shape = ObstacleShape::Box;
    desc.position = {0.0f, 0.0f, 0.0f};
    desc.halfExtents = {1.0f, 1.0f, 1.0f};

    // Without navmesh, update of invalid handle should return false
    EXPECT_FALSE(mgr.UpdateObstacle(InvalidObstacleHandle, desc));
    mgr.Clear();
}

TEST(NavObstacles_ConsoleList)
{
    auto& mgr = NavMeshObstacleManager::GetInstance();
    mgr.Clear();
    std::string list = mgr.Console_ListObstacles();
    EXPECT_FALSE(list.empty());
}
