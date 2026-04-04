// TestScreenCapture.cpp - Tests for Spark::Graphics::ScreenCapture
#include "TestFramework.h"
#include "Graphics/ScreenCapture.h"

// ============================================================================
// Initialization
// ============================================================================

TEST(ScreenCapture_Initialize)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize("TestScreenshots");
    EXPECT_EQ(cap.GetScreenshotCount(), 0u);
    EXPECT_FALSE(cap.IsRecording());
    EXPECT_EQ(cap.GetOutputDirectory(), std::string("TestScreenshots"));
    cap.Shutdown();
}

// ============================================================================
// Screenshot capture
// ============================================================================

TEST(ScreenCapture_TakeScreenshot)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize("TestScreenshots");

    // Create a 2x2 RGBA test image
    uint8_t pixels[16] = {
        255, 0, 0,   255, 0,   255, 0, 255, // Row 1: red, green
        0,   0, 255, 255, 255, 255, 0, 255  // Row 2: blue, yellow
    };

    auto result = cap.TakeScreenshot(pixels, 2, 2);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(result.width, 2u);
    EXPECT_EQ(result.height, 2u);
    EXPECT_EQ(cap.GetScreenshotCount(), 1u);
    cap.Shutdown();
}

TEST(ScreenCapture_TakeScreenshot_NullData)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize();

    auto result = cap.TakeScreenshot(nullptr, 100, 100);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(cap.GetScreenshotCount(), 0u);
    cap.Shutdown();
}

TEST(ScreenCapture_TakeScreenshot_ZeroDimensions)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize();

    uint8_t pixel[4] = {255, 255, 255, 255};
    auto result = cap.TakeScreenshot(pixel, 0, 0);
    EXPECT_FALSE(result.success);
    cap.Shutdown();
}

// ============================================================================
// Frame sequence
// ============================================================================

TEST(ScreenCapture_StartSequence)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize();

    EXPECT_TRUE(cap.StartSequence("test_seq"));
    EXPECT_TRUE(cap.IsRecording());
    EXPECT_EQ(cap.GetSequenceFrameCount(), 0u);

    // Can't start another while recording
    EXPECT_FALSE(cap.StartSequence("another"));

    cap.StopSequence();
    EXPECT_FALSE(cap.IsRecording());
    cap.Shutdown();
}

TEST(ScreenCapture_CaptureFrame)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize();

    cap.StartSequence("frames");

    uint8_t pixels[4] = {128, 128, 128, 255};
    auto result = cap.CaptureFrame(pixels, 1, 1);
    EXPECT_TRUE(result.success);
    EXPECT_EQ(cap.GetSequenceFrameCount(), 1u);

    auto result2 = cap.CaptureFrame(pixels, 1, 1);
    EXPECT_TRUE(result2.success);
    EXPECT_EQ(cap.GetSequenceFrameCount(), 2u);

    uint32_t totalFrames = cap.StopSequence();
    EXPECT_EQ(totalFrames, 2u);
    cap.Shutdown();
}

TEST(ScreenCapture_CaptureFrame_NoSequence)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize();

    uint8_t pixels[4] = {0, 0, 0, 255};
    auto result = cap.CaptureFrame(pixels, 1, 1);
    EXPECT_FALSE(result.success);
    cap.Shutdown();
}

// ============================================================================
// Console status
// ============================================================================

TEST(ScreenCapture_ConsoleStatus)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize();

    std::string status = cap.Console_GetStatus();
    EXPECT_TRUE(status.find("ScreenCapture") != std::string::npos);
    EXPECT_TRUE(status.find("0 screenshots") != std::string::npos);
    cap.Shutdown();
}

TEST(ScreenCapture_ConsoleStatus_Recording)
{
    auto& cap = Spark::Graphics::ScreenCapture::GetInstance();
    cap.Initialize();

    cap.StartSequence("test");
    std::string status = cap.Console_GetStatus();
    EXPECT_TRUE(status.find("recording") != std::string::npos);
    cap.StopSequence();
    cap.Shutdown();
}
