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

    panel.SetPlaying(true);
    panel.Update(0.5f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.5f, 0.0001f);

    panel.SetPlaying(false);
    panel.Update(1.0f);
    EXPECT_NEAR(panel.GetSimulationTime(), 0.5f, 0.0001f);
}
