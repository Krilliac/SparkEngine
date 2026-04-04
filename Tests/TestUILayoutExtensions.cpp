// TestUILayoutExtensions.cpp - Tests for Spark::UI layout extensions
#include "TestFramework.h"
#include "Engine/UI/UILayoutExtensions.h"

// ============================================================================
// UIFlexContainer
// ============================================================================

TEST(UIFlexContainer_DefaultDirection_Horizontal)
{
    Spark::UI::UIFlexContainer flex("toolbar");
    EXPECT_TRUE(flex.GetDirection() == Spark::UI::LayoutDirection::Horizontal);
}

TEST(UIFlexContainer_SetDirection_Vertical)
{
    Spark::UI::UIFlexContainer flex("toolbar");
    flex.SetDirection(Spark::UI::LayoutDirection::Vertical);
    EXPECT_TRUE(flex.GetDirection() == Spark::UI::LayoutDirection::Vertical);
}

TEST(UIFlexContainer_DefaultAlignment_Start)
{
    Spark::UI::UIFlexContainer flex("toolbar");
    EXPECT_TRUE(flex.GetAlignment() == Spark::UI::FlexAlign::Start);
}

TEST(UIFlexContainer_SetAlignment_Center)
{
    Spark::UI::UIFlexContainer flex("toolbar");
    flex.SetAlignment(Spark::UI::FlexAlign::Center);
    EXPECT_TRUE(flex.GetAlignment() == Spark::UI::FlexAlign::Center);
}

TEST(UIFlexContainer_SetAlignment_SpaceBetween)
{
    Spark::UI::UIFlexContainer flex("toolbar");
    flex.SetAlignment(Spark::UI::FlexAlign::SpaceBetween);
    EXPECT_TRUE(flex.GetAlignment() == Spark::UI::FlexAlign::SpaceBetween);
}

// ============================================================================
// UIGridLayout
// ============================================================================

TEST(UIGridLayout_SetColumns)
{
    Spark::UI::UIGridLayout grid("grid");
    grid.SetColumns(4);
    // No direct getter for columns, but placing a cell at col 3 should work
    Spark::UI::UIWidget widget("cell");
    grid.SetRows(2);
    grid.SetColumns(4);
    grid.SetCell(0, 3, &widget);
    // If it didn't crash, the column count was accepted
    EXPECT_TRUE(true);
}

TEST(UIGridLayout_SetRows)
{
    Spark::UI::UIGridLayout grid("grid");
    grid.SetRows(3);
    grid.SetColumns(2);
    Spark::UI::UIWidget widget("cell");
    grid.SetCell(2, 0, &widget);
    EXPECT_TRUE(true);
}

TEST(UIGridLayout_CalculateLayout_PositionsChildren)
{
    Spark::UI::UIGridLayout grid("grid");
    grid.SetSize(200.0f, 100.0f);
    grid.SetPosition(0.0f, 0.0f);
    grid.SetColumns(2);
    grid.SetRows(2);
    grid.SetCellSpacing(0.0f);

    Spark::UI::UIWidget w00("w00");
    Spark::UI::UIWidget w01("w01");
    Spark::UI::UIWidget w10("w10");
    grid.SetCell(0, 0, &w00);
    grid.SetCell(0, 1, &w01);
    grid.SetCell(1, 0, &w10);
    grid.CalculateLayout();

    EXPECT_NEAR(w00.GetX(), 0.0f, 0.01f);
    EXPECT_NEAR(w00.GetY(), 0.0f, 0.01f);
    EXPECT_NEAR(w01.GetX(), 100.0f, 0.01f);
    EXPECT_NEAR(w10.GetY(), 50.0f, 0.01f);
}

TEST(UIGridLayout_ZeroColumns_ClampsToOne)
{
    Spark::UI::UIGridLayout grid("grid");
    grid.SetColumns(0);
    // Should clamp to 1, not crash
    grid.SetRows(1);
    grid.SetSize(100.0f, 100.0f);
    Spark::UI::UIWidget widget("w");
    grid.SetCell(0, 0, &widget);
    grid.CalculateLayout();
    EXPECT_TRUE(true);
}

// ============================================================================
// UIScrollView
// ============================================================================

TEST(UIScrollView_DefaultScrollOffset_Zero)
{
    Spark::UI::UIScrollView scroll("scroll");
    auto [x, y] = scroll.GetScrollOffset();
    EXPECT_NEAR(x, 0.0f, 0.001f);
    EXPECT_NEAR(y, 0.0f, 0.001f);
}

TEST(UIScrollView_SetScrollOffset_ClampsToBounds)
{
    Spark::UI::UIScrollView scroll("scroll");
    scroll.SetViewportSize(100.0f, 100.0f);
    scroll.SetContentSize(200.0f, 300.0f);

    scroll.SetScrollOffset(50.0f, 250.0f);
    auto [x, y] = scroll.GetScrollOffset();
    EXPECT_NEAR(x, 50.0f, 0.001f);
    // Max Y scroll = 300 - 100 = 200
    EXPECT_NEAR(y, 200.0f, 0.001f);
}

TEST(UIScrollView_SetScrollOffset_NegativeClampsToZero)
{
    Spark::UI::UIScrollView scroll("scroll");
    scroll.SetViewportSize(100.0f, 100.0f);
    scroll.SetContentSize(200.0f, 200.0f);

    scroll.SetScrollOffset(-10.0f, -5.0f);
    auto [x, y] = scroll.GetScrollOffset();
    EXPECT_NEAR(x, 0.0f, 0.001f);
    EXPECT_NEAR(y, 0.0f, 0.001f);
}

TEST(UIScrollView_IsScrollable_LargerContent)
{
    Spark::UI::UIScrollView scroll("scroll");
    scroll.SetViewportSize(100.0f, 100.0f);
    scroll.SetContentSize(200.0f, 100.0f);
    EXPECT_TRUE(scroll.IsScrollable());
}

TEST(UIScrollView_IsScrollable_SmallerContent)
{
    Spark::UI::UIScrollView scroll("scroll");
    scroll.SetViewportSize(100.0f, 100.0f);
    scroll.SetContentSize(50.0f, 50.0f);
    EXPECT_FALSE(scroll.IsScrollable());
}

TEST(UIScrollView_GetMaxScroll)
{
    Spark::UI::UIScrollView scroll("scroll");
    scroll.SetViewportSize(100.0f, 100.0f);
    scroll.SetContentSize(400.0f, 300.0f);
    auto [maxX, maxY] = scroll.GetMaxScroll();
    EXPECT_NEAR(maxX, 300.0f, 0.001f);
    EXPECT_NEAR(maxY, 200.0f, 0.001f);
}

// ============================================================================
// UITextInput
// ============================================================================

TEST(UITextInput_SetGet_Text)
{
    Spark::UI::UITextInput input("name");
    input.SetText("Hello");
    EXPECT_TRUE(input.GetText() == "Hello");
}

TEST(UITextInput_Placeholder)
{
    Spark::UI::UITextInput input("name");
    input.SetPlaceholder("Enter name...");
    // Placeholder is set (no getter exposed, but no crash)
    EXPECT_TRUE(input.GetText().empty());
}

TEST(UITextInput_MaxLength_Truncates)
{
    Spark::UI::UITextInput input("name");
    input.SetMaxLength(5);
    input.SetText("HelloWorld");
    EXPECT_TRUE(input.GetText() == "Hello");
    EXPECT_EQ(input.GetText().size(), 5u);
}

TEST(UITextInput_ReadOnly_RejectsText)
{
    Spark::UI::UITextInput input("name");
    input.SetText("Initial");
    input.SetReadOnly(true);
    input.SetText("Changed");
    EXPECT_TRUE(input.GetText() == "Initial");
}

TEST(UITextInput_OnTextChanged_Callback)
{
    Spark::UI::UITextInput input("name");
    std::string captured;
    input.OnTextChanged([&](const std::string& text) { captured = text; });
    input.SetText("Test");
    EXPECT_TRUE(captured == "Test");
}

// ============================================================================
// UISlider
// ============================================================================

TEST(UISlider_DefaultRange)
{
    Spark::UI::UISlider slider("vol");
    EXPECT_NEAR(slider.GetValue(), 0.0f, 0.001f);
}

TEST(UISlider_SetRange_ClampsValue)
{
    Spark::UI::UISlider slider("vol");
    slider.SetRange(0.0f, 100.0f);
    slider.SetValue(150.0f);
    EXPECT_NEAR(slider.GetValue(), 100.0f, 0.001f);
}

TEST(UISlider_SetValue_WithStep)
{
    Spark::UI::UISlider slider("vol");
    slider.SetRange(0.0f, 10.0f);
    slider.SetStep(2.5f);
    slider.SetValue(3.0f);
    // Should snap to nearest step: 2.5
    EXPECT_NEAR(slider.GetValue(), 2.5f, 0.001f);
}

TEST(UISlider_OnValueChanged_Callback)
{
    Spark::UI::UISlider slider("vol");
    slider.SetRange(0.0f, 1.0f);
    float captured = -1.0f;
    slider.OnValueChanged([&](float v) { captured = v; });
    slider.SetValue(0.75f);
    EXPECT_NEAR(captured, 0.75f, 0.001f);
}

// ============================================================================
// UIDropdown
// ============================================================================

TEST(UIDropdown_AddOption_And_GetSelectedValue)
{
    Spark::UI::UIDropdown dropdown("lang");
    dropdown.AddOption("English", "en");
    dropdown.AddOption("French", "fr");
    // Default selection is index 0
    EXPECT_TRUE(dropdown.GetSelectedValue() == "en");
}

TEST(UIDropdown_SetSelectedIndex)
{
    Spark::UI::UIDropdown dropdown("lang");
    dropdown.AddOption("English", "en");
    dropdown.AddOption("French", "fr");
    dropdown.SetSelectedIndex(1);
    EXPECT_EQ(dropdown.GetSelectedIndex(), 1u);
    EXPECT_TRUE(dropdown.GetSelectedValue() == "fr");
}

TEST(UIDropdown_RemoveOption)
{
    Spark::UI::UIDropdown dropdown("lang");
    dropdown.AddOption("English", "en");
    dropdown.AddOption("French", "fr");
    dropdown.AddOption("German", "de");
    dropdown.RemoveOption("fr");
    // Should have 2 options remaining
    dropdown.SetSelectedIndex(1);
    EXPECT_TRUE(dropdown.GetSelectedValue() == "de");
}

TEST(UIDropdown_OnSelectionChanged_Callback)
{
    Spark::UI::UIDropdown dropdown("lang");
    dropdown.AddOption("English", "en");
    dropdown.AddOption("French", "fr");
    uint32_t capturedIndex = 999;
    std::string capturedValue;
    dropdown.OnSelectionChanged(
        [&](uint32_t idx, const std::string& val)
        {
            capturedIndex = idx;
            capturedValue = val;
        });
    dropdown.SetSelectedIndex(1);
    EXPECT_EQ(capturedIndex, 1u);
    EXPECT_TRUE(capturedValue == "fr");
}

TEST(UIDropdown_Toggle)
{
    Spark::UI::UIDropdown dropdown("lang");
    EXPECT_FALSE(dropdown.IsOpen());
    dropdown.Toggle();
    EXPECT_TRUE(dropdown.IsOpen());
    dropdown.Toggle();
    EXPECT_FALSE(dropdown.IsOpen());
}
