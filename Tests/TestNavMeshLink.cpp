// TestNavMeshLink.cpp — tests for Spark::AI::NavMeshLinkSystem
//
// Exercises the off-mesh navigation link registry directly. No
// external dependencies — the whole class is a header-only singleton
// backed by std::unordered_map.

#include "TestFramework.h"
#include "Engine/AI/NavMeshLink.h"

#include <cstdint>
#include <vector>

using namespace Spark::AI;

namespace
{
    NavLink MakeLink(XMFLOAT3 start, XMFLOAT3 end, NavLinkTraversalType type = NavLinkTraversalType::Jump)
    {
        NavLink link;
        link.startPos = start;
        link.endPos = end;
        link.radius = 0.5f;
        link.traversalType = type;
        link.traversalCost = 1.0f;
        link.bidirectional = true;
        link.enabled = true;
        return link;
    }
} // namespace

TEST(NavMeshLinkSystem_InitializeClearsLinks)
{
    auto& sys = NavMeshLinkSystem::GetInstance();
    sys.Initialize();
    sys.AddLink(MakeLink({0, 0, 0}, {5, 0, 0}));
    EXPECT_GT(sys.GetLinkCount(), static_cast<uint32_t>(0));

    sys.Initialize(); // Re-init wipes state.
    EXPECT_EQ(sys.GetLinkCount(), static_cast<uint32_t>(0));

    sys.Shutdown();
}

TEST(NavMeshLinkSystem_AddLinkAssignsIdAndStores)
{
    auto& sys = NavMeshLinkSystem::GetInstance();
    sys.Initialize();

    const uint32_t id = sys.AddLink(MakeLink({1, 0, 2}, {3, 0, 4}, NavLinkTraversalType::Climb));
    EXPECT_GT(id, static_cast<uint32_t>(0));
    EXPECT_EQ(sys.GetLinkCount(), static_cast<uint32_t>(1));

    const NavLink* found = sys.FindLink(id);
    EXPECT_NE(found, nullptr);
    if (found)
    {
        EXPECT_EQ(static_cast<int>(found->traversalType), static_cast<int>(NavLinkTraversalType::Climb));
        EXPECT_NEAR(found->startPos.x, 1.0f, 1e-5f);
        EXPECT_NEAR(found->endPos.z, 4.0f, 1e-5f);
    }

    sys.Shutdown();
}

TEST(NavMeshLinkSystem_RemoveLink)
{
    auto& sys = NavMeshLinkSystem::GetInstance();
    sys.Initialize();
    const uint32_t id = sys.AddLink(MakeLink({0, 0, 0}, {1, 0, 0}));
    EXPECT_EQ(sys.GetLinkCount(), static_cast<uint32_t>(1));

    sys.RemoveLink(id);
    EXPECT_EQ(sys.GetLinkCount(), static_cast<uint32_t>(0));
    EXPECT_EQ(sys.FindLink(id), nullptr);

    // Removing a non-existent link is a no-op.
    sys.RemoveLink(9999);
    EXPECT_EQ(sys.GetLinkCount(), static_cast<uint32_t>(0));

    sys.Shutdown();
}

TEST(NavMeshLinkSystem_EnableDisableLink)
{
    auto& sys = NavMeshLinkSystem::GetInstance();
    sys.Initialize();
    const uint32_t id = sys.AddLink(MakeLink({0, 0, 0}, {10, 0, 0}));

    sys.EnableLink(id, false);
    EXPECT_FALSE(sys.FindLink(id)->enabled);

    sys.EnableLink(id, true);
    EXPECT_TRUE(sys.FindLink(id)->enabled);

    sys.Shutdown();
}

TEST(NavMeshLinkSystem_FindLinksNearBidirectional)
{
    auto& sys = NavMeshLinkSystem::GetInstance();
    sys.Initialize();

    // Start at (0,0,0), end at (10,0,0) — both endpoints are candidates
    // because the link is bidirectional.
    sys.AddLink(MakeLink({0, 0, 0}, {10, 0, 0}));
    // Unidirectional link starting at (100,0,0).
    NavLink oneWay = MakeLink({100, 0, 0}, {110, 0, 0});
    oneWay.bidirectional = false;
    sys.AddLink(oneWay);

    // Search near origin — should pick up the first link's start.
    auto near0 = sys.FindLinksNear({0, 0, 0}, 1.0f);
    EXPECT_GT(near0.size(), static_cast<size_t>(0));

    // Search near (10,0,0) — bidirectional link means this matches too.
    auto near10 = sys.FindLinksNear({10, 0, 0}, 1.0f);
    EXPECT_GT(near10.size(), static_cast<size_t>(0));

    // Search near (110,0,0) — the unidirectional link ends there but
    // isn't bidirectional, so it should NOT be returned.
    auto near110 = sys.FindLinksNear({110, 0, 0}, 1.0f);
    EXPECT_EQ(near110.size(), static_cast<size_t>(0));

    // Disabled links are excluded.
    sys.Initialize();
    const uint32_t id = sys.AddLink(MakeLink({0, 0, 0}, {10, 0, 0}));
    sys.EnableLink(id, false);
    auto disabled = sys.FindLinksNear({0, 0, 0}, 1.0f);
    EXPECT_EQ(disabled.size(), static_cast<size_t>(0));

    sys.Shutdown();
}
