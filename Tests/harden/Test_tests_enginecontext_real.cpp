/**
 * @file Test_tests_enginecontext_real.cpp
 * @brief Real-class tests for the shipped EngineContext registry.
 *
 * These tests drive a local EngineContext instance (default-constructed, so it
 * never touches the process-wide singleton). EngineContext is a locator only;
 * subsystem lifecycle belongs to EngineRuntime (OD-01). Together with the
 * EngineContextReal_* tests in TestSparkGameFPSMirrorCompanionsReal.cpp they
 * replace the ServiceLocator mirror that RDY-010 retired.
 */

#include "TestFramework.h"
#include "Core/EngineContext.h"

namespace
{
    // Distinct, complete tag types used as fake subsystems. Each gets its own
    // GetTypeId<T>() marker, exactly like a real subsystem class.
    struct SysA
    {
        int value = 0;
    };

    struct SysB
    {
        int value = 0;
    };
} // namespace

TEST(EngineContextReal_RegisterSystemNullptrUnregisters)
{
    EngineContext ctx;
    SysA a;

    ctx.RegisterSystem<SysA>(&a);
    ASSERT_TRUE(ctx.GetSystem<SysA>() == &a);

    ctx.RegisterSystem<SysA>(nullptr);
    EXPECT_TRUE(ctx.GetSystem<SysA>() == nullptr);
}

TEST(EngineContextReal_ReRegistrationReplacesOnlyThatType)
{
    EngineContext ctx;
    SysA first;
    first.value = 10;
    SysA replacement;
    replacement.value = 20;
    SysB other;
    other.value = 30;

    ASSERT_TRUE(ctx.RegisterSystem<SysA>(&first));
    ASSERT_TRUE(ctx.RegisterSystem<SysB>(&other));
    ASSERT_TRUE(ctx.RegisterSystem<SysA>(&replacement));

    // Lookups through a const context see the same registry.
    const EngineContext& view = ctx;
    ASSERT_TRUE(view.GetSystem<SysA>() == &replacement);
    EXPECT_EQ(view.GetSystem<SysA>()->value, 20);
    ASSERT_TRUE(view.GetSystem<SysB>() == &other);
    EXPECT_EQ(view.GetSystem<SysB>()->value, 30);
}
