/**
 * @file TestGameViewPanel.cpp
 * @brief Play-state ownership tests for the editor Game View simulation.
 */

#include "TestFramework.h"
#include "Panels/GameViewPanel.h"

TEST(GameViewPanel_SimulationClockOnlyAdvancesWhilePlaying)
{
    SparkEditor::GameViewPanel panel;

    panel.Update(1.0f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.0f, 0.0001f);

    panel.SetFPSHUDPreviewEnabled(true);
    panel.SetPlaying(true);
    panel.Update(0.5f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.5f, 0.0001f);

    panel.SetPlaying(false);
    panel.Update(1.0f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.5f, 0.0001f);
}

TEST(GameViewPanel_FPSHUDPreviewIsExplicitlyProjectScoped)
{
    SparkEditor::GameViewPanel panel;

    EXPECT_FALSE(panel.IsFPSHUDPreviewAvailable());
    EXPECT_FALSE(panel.IsFPSHUDPreviewEnabled());
    panel.SetFPSHUDPreviewVisible(true);
    EXPECT_FALSE(panel.IsFPSHUDPreviewEnabled());

    panel.SetPlaying(true);
    panel.Update(1.0f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.0f, 0.0001f);
    EXPECT_FALSE(panel.ShouldShowInputCaptureHint());

    panel.SetFPSHUDPreviewEnabled(true);
    EXPECT_TRUE(panel.IsFPSHUDPreviewAvailable());
    EXPECT_TRUE(panel.IsFPSHUDPreviewEnabled());
    EXPECT_TRUE(panel.ShouldShowInputCaptureHint());
    EXPECT_FALSE(panel.ShouldShowInputReleaseHint());
    panel.Update(0.5f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.5f, 0.0001f);

    // A new FPS project must not inherit simulated combat/camera time from
    // the previous FPS project merely because preview availability stayed on.
    panel.SetFPSHUDPreviewEnabled(true);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.0f, 0.0001f);

    panel.SetFPSHUDPreviewEnabled(false);
    EXPECT_FALSE(panel.IsFPSHUDPreviewAvailable());
    EXPECT_FALSE(panel.IsFPSHUDPreviewEnabled());
    EXPECT_FALSE(panel.ShouldShowInputCaptureHint());
    panel.Update(1.0f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.0f, 0.0001f);

    panel.SetFPSHUDPreviewEnabled(true);
    panel.Update(0.25f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.25f, 0.0001f);
}

TEST(GameViewPanel_InputHintsDistinguishCaptureFromRelease)
{
    using Hint = SparkEditor::GameViewPanel::InputCaptureHint;
    using Panel = SparkEditor::GameViewPanel;

    EXPECT_TRUE(Panel::ResolveInputCaptureHint(false, true, false) == Hint::None);
    EXPECT_TRUE(Panel::ResolveInputCaptureHint(true, false, false) == Hint::None);
    EXPECT_TRUE(Panel::ResolveInputCaptureHint(true, true, false) == Hint::Capture);
    EXPECT_TRUE(Panel::ResolveInputCaptureHint(true, true, true) == Hint::Release);
}
