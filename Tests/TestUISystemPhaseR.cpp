/**
 * @file TestUISystemPhaseR.cpp
 * @brief Phase R — integration tests for UISystem's UICompositor activation
 *
 * Phase R wires `Spark::Graphics::UICompositor` as a per-instance
 * member of `Spark::UI::UISystem`. These tests cover the portable
 * accessor surface so they execute on every platform's CI.
 *
 * CPU oracle tests for `UICompositor` itself live in
 * `TestUICompositor.cpp`; Phase R's integration tests focus on the
 * lifecycle hook contract — that UISystem's Initialize / OnResize /
 * Render calls drive the compositor's state correctly.
 */

#include "TestFramework.h"

#include "../SparkEngine/Source/Engine/UI/UISystem.h"

using Spark::Graphics::CompositeOp;
using Spark::Graphics::CompositeRequest;
using Spark::UI::UISystem;

// =========================================================================
// Lifecycle: compositor follows the UI system
// =========================================================================

TEST(PhaseR_Compositor_InitializedWithUISystem)
{
    UISystem ui;
    // Pre-initialize: compositor default state (not initialised).
    EXPECT_FALSE(ui.GetCompositor().IsInitialized());

    ui.Initialize(1920, 1080);
    EXPECT_TRUE(ui.GetCompositor().IsInitialized());
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetStackDepth()), 0);
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetPoolSize()), 0);
}

TEST(PhaseR_Compositor_HandlesNonPositiveDimensions)
{
    UISystem ui;
    // Passing 0/0 must clamp to 1/1 so the compositor still initialises.
    ui.Initialize(0, 0);
    EXPECT_TRUE(ui.GetCompositor().IsInitialized());
}

// =========================================================================
// Push / pop via the UISystem-owned compositor
// =========================================================================

TEST(PhaseR_Compositor_AcceptsPushPopViaAccessor)
{
    UISystem ui;
    ui.Initialize(1280, 720);

    CompositeRequest req;
    req.operation = CompositeOp::AlphaBlend;
    req.width = 640;
    req.height = 360;
    req.opacity = 0.5f;

    int idx = ui.GetCompositor().BeginComposite(req);
    EXPECT_GE(idx, 0);
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetStackDepth()), 1);

    ui.GetCompositor().EndComposite();
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetStackDepth()), 0);
}

// =========================================================================
// Render() advances the compositor frame counter
// =========================================================================

TEST(PhaseR_Render_ReclaimsStaleCompositorEntries)
{
    UISystem ui;
    ui.Initialize(1024, 768);

    // Create and release a pool entry.
    CompositeRequest req;
    req.operation = CompositeOp::GaussianBlur;
    req.width = 512;
    req.height = 512;
    req.blurRadius = 3.0f;
    ui.GetCompositor().BeginComposite(req);
    ui.GetCompositor().EndComposite();
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetPoolSize()), 1);

    // Render 15 frames without any push / pop — after the 10-frame
    // idle threshold the pool entry must be reclaimed.
    for (int i = 0; i < 15; ++i)
    {
        ui.Render();
    }
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetPoolSize()), 0);
}

TEST(PhaseR_Render_ClearsLeftoverStack)
{
    UISystem ui;
    ui.Initialize(1024, 768);

    // Simulate a mid-frame bail-out: push composites, forget to pop.
    CompositeRequest req;
    req.operation = CompositeOp::AlphaBlend;
    req.width = 256;
    req.height = 256;
    ui.GetCompositor().BeginComposite(req);
    ui.GetCompositor().BeginComposite(req);
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetStackDepth()), 2);

    ui.Render(); // BeginFrame inside Render must clear the stack
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetStackDepth()), 0);
}

// =========================================================================
// OnResize re-initialises the compositor
// =========================================================================

TEST(PhaseR_OnResize_ReinitializesCompositor)
{
    UISystem ui;
    ui.Initialize(1920, 1080);

    // Create a pool entry sized for 1920x1080.
    CompositeRequest req;
    req.operation = CompositeOp::AlphaBlend;
    req.width = 960;
    req.height = 540;
    ui.GetCompositor().BeginComposite(req);
    ui.GetCompositor().EndComposite();
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetPoolSize()), 1);

    // Resize drops the pool — the compositor re-initialises at the
    // new dimensions and forgets the old metadata.
    ui.OnResize(1280, 720);
    EXPECT_TRUE(ui.GetCompositor().IsInitialized());
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetPoolSize()), 0);
    EXPECT_EQ(static_cast<int>(ui.GetCompositor().GetStackDepth()), 0);
}

TEST(PhaseR_OnResize_HandlesZeroDimensions)
{
    UISystem ui;
    ui.Initialize(800, 600);
    ui.OnResize(0, 0); // clamps to 1/1
    EXPECT_TRUE(ui.GetCompositor().IsInitialized());
}
