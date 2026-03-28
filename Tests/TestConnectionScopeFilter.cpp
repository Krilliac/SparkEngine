// TestConnectionScopeFilter.cpp - Tests for network visibility filtering

#include "TestFramework.h"
#include "Engine/Networking/ConnectionScopeFilter.h"

using namespace Spark::Net;

TEST(ScopeFilter_InitiallyEmpty)
{
    auto& filter = ConnectionScopeFilter::GetInstance();
    filter.Shutdown();
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(0));
}

TEST(ScopeFilter_SetAndRemoveScope)
{
    auto& filter = ConnectionScopeFilter::GetInstance();
    filter.Shutdown();

    ConnectionScope scope;
    scope.areaId = 1;
    scope.position = {0.0f, 0.0f, 0.0f};
    scope.radius = 100.0f;
    filter.SetScope(1, scope);
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(1));

    filter.RemoveConnection(1);
    EXPECT_EQ(filter.GetConnectionCount(), static_cast<size_t>(0));
}

TEST(ScopeFilter_EntityInScopeByDistance)
{
    auto& filter = ConnectionScopeFilter::GetInstance();
    filter.Shutdown();

    ConnectionScope scope;
    scope.areaId = 0; // 0 = all areas
    scope.position = {0.0f, 0.0f, 0.0f};
    scope.radius = 50.0f;
    scope.teamMask = 0xFFFFFFFF;
    scope.visibilityMask = 0xFFFFFFFF;
    filter.SetScope(1, scope);

    // Entity within radius
    EXPECT_TRUE(filter.IsEntityInScope(1, {10.0f, 0.0f, 0.0f}, 0, 0x01, 0x01));

    // Entity outside radius
    EXPECT_FALSE(filter.IsEntityInScope(1, {200.0f, 0.0f, 0.0f}, 0, 0x01, 0x01));

    filter.Shutdown();
}

TEST(ScopeFilter_EntityFilteredByArea)
{
    auto& filter = ConnectionScopeFilter::GetInstance();
    filter.Shutdown();

    ConnectionScope scope;
    scope.areaId = 5; // Only area 5
    scope.position = {0.0f, 0.0f, 0.0f};
    scope.radius = 1000.0f;
    scope.teamMask = 0xFFFFFFFF;
    scope.visibilityMask = 0xFFFFFFFF;
    filter.SetScope(1, scope);

    EXPECT_TRUE(filter.IsEntityInScope(1, {0.0f, 0.0f, 0.0f}, 5, 0x01, 0x01));
    EXPECT_FALSE(filter.IsEntityInScope(1, {0.0f, 0.0f, 0.0f}, 3, 0x01, 0x01));

    filter.Shutdown();
}

TEST(ScopeFilter_ConsoleStatus)
{
    auto& filter = ConnectionScopeFilter::GetInstance();
    filter.Shutdown();
    std::string status = filter.Console_GetStatus();
    EXPECT_FALSE(status.empty());
}
