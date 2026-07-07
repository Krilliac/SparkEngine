/**
 * @file Test_ui-2d_ui.cpp
 * @brief Hardening tests for the runtime UI system.
 *
 * Covers two audit findings:
 *  - UIDropdown::RemoveOption corrupted the selected index when an option
 *    before the selection was removed.
 *  - UIPanel auto-layout positioned children in panel-local space while
 *    HandleClick forwarded absolute coordinates, so clicks missed for any
 *    panel not at the origin.
 */

#include "../TestFramework.h"
#include "../../SparkEngine/Source/Engine/UI/UILayoutExtensions.h"

using namespace Spark::UI;

// ============================================================================
// UIDropdown::RemoveOption
// ============================================================================

TEST(UIDropdown_RemoveBeforeSelectedKeepsSelection)
{
    UIDropdown dd("dd");
    dd.AddOption("A", "a");
    dd.AddOption("B", "b");
    dd.AddOption("C", "c");
    dd.SetSelectedIndex(2); // select "c"

    bool changedFired = false;
    dd.OnSelectionChanged([&](uint32_t, const std::string&) { changedFired = true; });

    dd.RemoveOption("a"); // remove an option BEFORE the selection

    // "c" is still selected; only its index shifted from 2 to 1.
    EXPECT_EQ(dd.GetSelectedIndex(), 1u);
    EXPECT_EQ(dd.GetSelectedValue(), std::string("c"));
    // The effective selection did not change, so no notification should fire.
    EXPECT_FALSE(changedFired);
}

TEST(UIDropdown_RemoveSelectedFallsBackAndNotifies)
{
    UIDropdown dd("dd");
    dd.AddOption("A", "a");
    dd.AddOption("B", "b");
    dd.AddOption("C", "c");
    dd.SetSelectedIndex(1); // select "b"

    bool changedFired = false;
    std::string newValue;
    dd.OnSelectionChanged([&](uint32_t, const std::string& v)
                          {
                              changedFired = true;
                              newValue = v;
                          });

    dd.RemoveOption("b"); // remove the SELECTED option

    // Selection falls back to a valid index and listeners are notified.
    EXPECT_EQ(dd.GetSelectedIndex(), 0u);
    EXPECT_EQ(dd.GetSelectedValue(), std::string("a"));
    EXPECT_TRUE(changedFired);
    EXPECT_EQ(newValue, std::string("a"));
}

// ============================================================================
// UIPanel click hit-testing honours the panel origin
// ============================================================================

TEST(UIPanel_ClickHitsChildOnOffsetPanel)
{
    UIPanel panel("panel");
    panel.SetVisible(true);
    panel.SetPosition(100.0f, 100.0f);
    panel.SetLayout(LayoutDirection::Vertical);
    panel.SetPadding(10.0f);
    panel.SetSpacing(5.0f);

    UIButton* button = panel.CreateButton("btn", "Go");
    button->SetVisible(true);
    button->SetSize(50.0f, 20.0f);

    bool clicked = false;
    button->OnClick([&]() { clicked = true; });

    // Run auto-layout: the button lands at absolute (110, 110), size 50x20,
    // covering x in [110,160], y in [110,130].
    panel.Update(0.0f);

    // A click inside the button's absolute rect must hit it.
    EXPECT_TRUE(panel.HandleClick(120.0f, 120.0f));
    EXPECT_TRUE(clicked);

    // A click at the old (buggy) panel-local position must NOT hit.
    clicked = false;
    EXPECT_FALSE(panel.HandleClick(15.0f, 15.0f));
    EXPECT_FALSE(clicked);
}
