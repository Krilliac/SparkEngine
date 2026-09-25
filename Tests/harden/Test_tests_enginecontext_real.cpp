/**
 * @file Test_tests_enginecontext_real.cpp
 * @brief Real-class tests for the shipped EngineContext registry.
 *
 * TestEngineContext.cpp exercises a standalone ServiceLocator reimplementation,
 * so these tests drive a local EngineContext instance (default-constructed, so
 * it never touches the process-wide singleton). EngineContext is a locator
 * only; subsystem lifecycle belongs to EngineRuntime (OD-01).
 */

#include "TestFramework.h"
#include "Core/EngineContext.h"

namespace
{
    // Distinct, complete tag type used as a fake subsystem. It gets its own
    // GetTypeId<T>() marker, exactly like a real subsystem class.
    struct SysA
    {
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
