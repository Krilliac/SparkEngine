/**
 * @file TestUISystem.cpp
 * @brief Tests for Spark::UI::UISystem
 */

#include "TestFramework.h"
#include "Engine/UI/UISystem.h"

TEST(UI_CreateCanvas)
{
    Spark::UI::UISystem ui;
    ui.Initialize(1920, 1080);

    EXPECT_EQ(ui.GetCanvas().GetWidth(), 1920);
    EXPECT_EQ(ui.GetCanvas().GetHeight(), 1080);
    EXPECT_TRUE(ui.IsVisible());
}

TEST(UI_CreatePanelAndWidgets)
{
    Spark::UI::UICanvas canvas;
    canvas.Initialize(1920, 1080);

    auto* panel = canvas.CreatePanel("HUD");
    ASSERT_TRUE(panel != nullptr);
    EXPECT_EQ(panel->GetName(), std::string("HUD"));

    auto* label = panel->CreateLabel("health_text", "Health: 100");
    ASSERT_TRUE(label != nullptr);
    EXPECT_EQ(label->GetText(), std::string("Health: 100"));

    auto* bar = panel->CreateProgressBar("health_bar");
    ASSERT_TRUE(bar != nullptr);
    bar->SetValue(0.75f);
    EXPECT_NEAR(bar->GetValue(), 0.75f, 0.001f);
}

TEST(UI_FindWidget)
{
    Spark::UI::UICanvas canvas;
    canvas.Initialize(800, 600);

    auto* panel = canvas.CreatePanel("main");
    panel->CreateLabel("title", "Game Title");
    panel->CreateButton("play_btn", "Play");

    auto* found = canvas.FindWidget("title");
    ASSERT_TRUE(found != nullptr);
    EXPECT_EQ(found->GetName(), std::string("title"));

    auto* notFound = canvas.FindWidget("nonexistent");
    EXPECT_TRUE(notFound == nullptr);
}

TEST(UI_ButtonClick)
{
    Spark::UI::UICanvas canvas;
    canvas.Initialize(800, 600);

    auto* panel = canvas.CreatePanel("menu");
    auto* btn = panel->CreateButton("click_me", "Click");
    btn->SetPosition(100, 100);
    btn->SetSize(200, 50);

    bool clicked = false;
    btn->OnClick([&]() { clicked = true; });

    // Click inside button bounds
    btn->HandleClick(150, 120);
    EXPECT_TRUE(clicked);
}

TEST(UI_ProgressBarClamp)
{
    Spark::UI::UIProgressBar bar("test");
    bar.SetValue(1.5f);
    EXPECT_NEAR(bar.GetValue(), 1.0f, 0.001f);

    bar.SetValue(-0.5f);
    EXPECT_NEAR(bar.GetValue(), 0.0f, 0.001f);
}

TEST(UI_Resize)
{
    Spark::UI::UISystem ui;
    ui.Initialize(1920, 1080);
    ui.OnResize(2560, 1440);

    EXPECT_EQ(ui.GetCanvas().GetWidth(), 2560);
    EXPECT_EQ(ui.GetCanvas().GetHeight(), 1440);
}
