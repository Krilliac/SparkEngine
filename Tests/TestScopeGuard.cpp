// TestScopeGuard.cpp - Tests for Spark::ScopeExit, ScopeSuccess, ScopeFail
#include "TestFramework.h"
#include "Utils/ScopeGuard.h"

// ============================================================================
// ScopeExit — fires unconditionally
// ============================================================================

TEST(ScopeGuard_ScopeExit_Fires)
{
    int counter = 0;
    {
        auto guard = Spark::MakeScopeExit([&] { ++counter; });
    }
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuard_ScopeExit_FiresOnReturn)
{
    int counter = 0;
    auto fn = [&]()
    {
        auto guard = Spark::MakeScopeExit([&] { ++counter; });
        return; // guard fires here
    };
    fn();
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuard_ScopeExit_Dismiss)
{
    int counter = 0;
    {
        auto guard = Spark::MakeScopeExit([&] { ++counter; });
        guard.Dismiss();
    }
    EXPECT_EQ(counter, 0);
}

TEST(ScopeGuard_ScopeExit_Arm_After_Dismiss)
{
    int counter = 0;
    {
        auto guard = Spark::MakeScopeExit([&] { ++counter; });
        guard.Dismiss();
        EXPECT_FALSE(guard.IsActive());
        guard.Arm();
        EXPECT_TRUE(guard.IsActive());
    }
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuard_ScopeExit_IsActive_Default)
{
    auto guard = Spark::MakeScopeExit([] {});
    EXPECT_TRUE(guard.IsActive());
    guard.Dismiss();
    EXPECT_FALSE(guard.IsActive());
}

TEST(ScopeGuard_ScopeExit_Move)
{
    int counter = 0;
    {
        auto g1 = Spark::MakeScopeExit([&] { ++counter; });
        auto g2 = std::move(g1);
        // g1 is dismissed after move; g2 owns the callback
    }
    EXPECT_EQ(counter, 1); // fired exactly once from g2
}

TEST(ScopeGuard_ScopeExit_MultipleCallbacks)
{
    int a = 0, b = 0;
    {
        auto ga = Spark::MakeScopeExit([&] { ++a; });
        auto gb = Spark::MakeScopeExit([&] { ++b; });
    }
    EXPECT_EQ(a, 1);
    EXPECT_EQ(b, 1);
}

// ============================================================================
// ScopeSuccess — fires only on normal exit
// ============================================================================

TEST(ScopeGuard_ScopeSuccess_FiresOnNormalExit)
{
    int counter = 0;
    {
        auto guard = Spark::MakeScopeSuccess([&] { ++counter; });
        // No exception — should fire
    }
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuard_ScopeSuccess_DoesNotFireOnException)
{
    int counter = 0;
    try
    {
        auto guard = Spark::MakeScopeSuccess([&] { ++counter; });
        throw std::runtime_error("test");
    }
    catch (...)
    {
    }
    EXPECT_EQ(counter, 0);
}

TEST(ScopeGuard_ScopeSuccess_Dismiss)
{
    int counter = 0;
    {
        auto guard = Spark::MakeScopeSuccess([&] { ++counter; });
        guard.Dismiss();
    }
    EXPECT_EQ(counter, 0);
}

// ============================================================================
// ScopeFail — fires only on exception
// ============================================================================

TEST(ScopeGuard_ScopeFail_DoesNotFireOnNormalExit)
{
    int counter = 0;
    {
        auto guard = Spark::MakeScopeFail([&] { ++counter; });
    }
    EXPECT_EQ(counter, 0);
}

TEST(ScopeGuard_ScopeFail_FiresOnException)
{
    int counter = 0;
    try
    {
        auto guard = Spark::MakeScopeFail([&] { ++counter; });
        throw std::runtime_error("test");
    }
    catch (...)
    {
    }
    EXPECT_EQ(counter, 1);
}

TEST(ScopeGuard_ScopeFail_Dismiss)
{
    int counter = 0;
    try
    {
        auto guard = Spark::MakeScopeFail([&] { ++counter; });
        guard.Dismiss();
        throw std::runtime_error("test");
    }
    catch (...)
    {
    }
    EXPECT_EQ(counter, 0);
}
