// TestFontSystem.cpp - Tests for Spark::Text::FontSystem
#include "TestFramework.h"
#include "Engine/Text/FontSystem.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(FontSystem_Initialize)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    EXPECT_EQ(fs.GetFontCount(), static_cast<size_t>(0));
    EXPECT_EQ(fs.GetActiveFont(), Spark::Text::INVALID_FONT_ID);
    fs.Shutdown();
}

TEST(FontSystem_Shutdown_ClearsState)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    fs.LoadFont("test.ttf", 24.0f);
    fs.Shutdown();
    EXPECT_EQ(fs.GetFontCount(), static_cast<size_t>(0));
}

// ============================================================================
// Font loading
// ============================================================================

TEST(FontSystem_LoadFont)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("Assets/Fonts/Roboto.ttf", 24.0f);
    EXPECT_TRUE(id != Spark::Text::INVALID_FONT_ID);
    EXPECT_EQ(fs.GetFontCount(), static_cast<size_t>(1));
    EXPECT_EQ(fs.GetActiveFont(), id); // First loaded becomes active
    fs.Shutdown();
}

TEST(FontSystem_LoadFont_EmptyPath_Fails)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("", 24.0f);
    EXPECT_EQ(id, Spark::Text::INVALID_FONT_ID);
    fs.Shutdown();
}

TEST(FontSystem_LoadFont_ZeroSize_Fails)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("test.ttf", 0.0f);
    EXPECT_EQ(id, Spark::Text::INVALID_FONT_ID);
    fs.Shutdown();
}

TEST(FontSystem_LoadMultipleFonts)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id1 = fs.LoadFont("font1.ttf", 16.0f);
    auto id2 = fs.LoadFont("font2.ttf", 32.0f);
    EXPECT_TRUE(id1 != id2);
    EXPECT_EQ(fs.GetFontCount(), static_cast<size_t>(2));
    fs.Shutdown();
}

TEST(FontSystem_UnloadFont)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("test.ttf", 24.0f);
    fs.UnloadFont(id);
    EXPECT_EQ(fs.GetFontCount(), static_cast<size_t>(0));
    fs.Shutdown();
}

// ============================================================================
// Font info
// ============================================================================

TEST(FontSystem_GetFontInfo)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("Assets/Fonts/Roboto.ttf", 24.0f);
    auto* info = fs.GetFontInfo(id);
    ASSERT_TRUE(info != nullptr);
    EXPECT_EQ(info->fontSize, 24.0f);
    EXPECT_EQ(info->name, std::string("Roboto"));
    EXPECT_TRUE(info->glyphCount > 0); // ASCII preloaded
    fs.Shutdown();
}

// ============================================================================
// Text layout
// ============================================================================

TEST(FontSystem_LayoutText_Basic)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("test.ttf", 24.0f);

    Spark::Text::TextStyle style;
    style.fontId = id;
    style.fontSize = 24.0f;

    auto quads = fs.LayoutText("Hello", 100.0f, 200.0f, style);
    EXPECT_EQ(quads.size(), static_cast<size_t>(5)); // 5 visible characters
    EXPECT_TRUE(quads[0].x0 >= 100.0f);              // Starts at x position
    fs.Shutdown();
}

TEST(FontSystem_LayoutText_Space)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("test.ttf", 24.0f);

    Spark::Text::TextStyle style;
    style.fontId = id;
    style.fontSize = 24.0f;

    // Space generates no quad (zero-size glyph)
    auto quads = fs.LayoutText("A B", 0, 0, style);
    EXPECT_EQ(quads.size(), static_cast<size_t>(2)); // 'A' and 'B', space has no visible quad
    fs.Shutdown();
}

// ============================================================================
// Text measurement
// ============================================================================

TEST(FontSystem_MeasureText)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("test.ttf", 24.0f);

    Spark::Text::TextStyle style;
    style.fontId = id;
    style.fontSize = 24.0f;

    auto bounds = fs.MeasureText("Hello", style);
    EXPECT_TRUE(bounds.width > 0.0f);
    EXPECT_TRUE(bounds.height > 0.0f);
    EXPECT_EQ(bounds.lineCount, static_cast<uint32_t>(1));
    fs.Shutdown();
}

TEST(FontSystem_MeasureText_MultiLine)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    auto id = fs.LoadFont("test.ttf", 24.0f);

    Spark::Text::TextStyle style;
    style.fontId = id;
    style.fontSize = 24.0f;

    auto bounds = fs.MeasureText("Line1\nLine2\nLine3", style);
    EXPECT_EQ(bounds.lineCount, static_cast<uint32_t>(3));
    fs.Shutdown();
}

TEST(FontSystem_ConsoleGetStatus)
{
    auto& fs = Spark::Text::FontSystem::GetInstance();
    fs.Initialize();
    fs.LoadFont("test.ttf", 24.0f);
    auto status = fs.Console_GetStatus();
    EXPECT_TRUE(status.find("Fonts loaded: 1") != std::string::npos);
    fs.Shutdown();
}
