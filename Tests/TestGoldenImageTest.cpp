// TestGoldenImageTest.cpp - Tests for Spark::GoldenImageTestRunner
#include "TestFramework.h"
#include "Utils/GoldenImageTest.h"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <vector>

namespace
{
    class FixedCapture final : public Spark::IGoldenImageCapture
    {
      public:
        explicit FixedCapture(const std::vector<uint8_t>& pixels) : m_pixels(pixels) {}

        [[nodiscard]] std::vector<uint8_t> CaptureFramebuffer(uint32_t, uint32_t) override { return m_pixels; }

      private:
        std::vector<uint8_t> m_pixels;
    };
} // namespace

// ============================================================================
// CompareImages — identical images
// ============================================================================

TEST(GoldenImageTest_CompareImages_Identical)
{
    constexpr uint32_t w = 4;
    constexpr uint32_t h = 4;
    std::vector<uint8_t> pixels(w * h * 4, 128);

    auto result = Spark::GoldenImageTestRunner::CompareImages(pixels.data(), pixels.data(), w, h, 10.0f);

    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.differentPixels, 0u);
    EXPECT_NEAR(result.percentDifferent, 0.0f, 0.001f);
    EXPECT_EQ(result.totalPixels, w * h);
}

// ============================================================================
// CompareImages — different images
// ============================================================================

TEST(GoldenImageTest_CompareImages_Different)
{
    constexpr uint32_t w = 2;
    constexpr uint32_t h = 2;
    std::vector<uint8_t> golden(w * h * 4, 0);
    std::vector<uint8_t> actual(w * h * 4, 255);

    auto result = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h, 10.0f);

    EXPECT_FALSE(result.matched);
    EXPECT_EQ(result.differentPixels, w * h);
    EXPECT_NEAR(result.percentDifferent, 100.0f, 0.01f);
    EXPECT_GT(result.maxPixelDistance, 0.0f);
}

TEST(GoldenImageTest_CompareImages_PartialDifference)
{
    constexpr uint32_t w = 2;
    constexpr uint32_t h = 1;
    // 2 pixels: first identical, second different
    std::vector<uint8_t> golden = {100, 100, 100, 255, 200, 200, 200, 255};
    std::vector<uint8_t> actual = {100, 100, 100, 255, 0, 0, 0, 255};

    auto result = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h, 10.0f);

    EXPECT_EQ(result.totalPixels, 2u);
    EXPECT_EQ(result.differentPixels, 1u);
    EXPECT_NEAR(result.percentDifferent, 50.0f, 0.01f);
}

// ============================================================================
// CompareImages — percentDifferent calculation
// ============================================================================

TEST(GoldenImageTest_CompareImages_PercentDifferent)
{
    constexpr uint32_t w = 4;
    constexpr uint32_t h = 1;
    // 4 pixels, make 1 differ significantly
    std::vector<uint8_t> golden(w * h * 4, 50);
    std::vector<uint8_t> actual(w * h * 4, 50);
    // Change the last pixel drastically
    actual[(w - 1) * 4 + 0] = 255;
    actual[(w - 1) * 4 + 1] = 255;
    actual[(w - 1) * 4 + 2] = 255;

    auto result = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h, 10.0f);

    EXPECT_EQ(result.totalPixels, 4u);
    EXPECT_EQ(result.differentPixels, 1u);
    EXPECT_NEAR(result.percentDifferent, 25.0f, 0.01f);
}

// ============================================================================
// CompareImages — tolerance threshold
// ============================================================================

TEST(GoldenImageTest_CompareImages_WithinTolerance)
{
    constexpr uint32_t w = 1;
    constexpr uint32_t h = 1;
    std::vector<uint8_t> golden = {100, 100, 100, 255};
    std::vector<uint8_t> actual = {102, 101, 99, 255};

    // Small difference, high tolerance -> should match
    auto result = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h, 10.0f);

    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.differentPixels, 0u);
}

TEST(GoldenImageTest_CompareImages_ExceedsTolerance)
{
    constexpr uint32_t w = 1;
    constexpr uint32_t h = 1;
    std::vector<uint8_t> golden = {100, 100, 100, 255};
    std::vector<uint8_t> actual = {200, 200, 200, 255};

    // Large difference, low tolerance -> should not match
    auto result = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h, 5.0f);

    EXPECT_FALSE(result.matched);
    EXPECT_EQ(result.differentPixels, 1u);
}

TEST(GoldenImageTest_CompareImages_NonFiniteThresholdFailsClosed)
{
    constexpr uint32_t w = 1;
    constexpr uint32_t h = 1;
    const std::vector<uint8_t> golden = {0, 0, 0, 255};
    const std::vector<uint8_t> actual = {255, 0, 0, 255};

    const auto nanResult = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h,
                                                                       std::numeric_limits<float>::quiet_NaN());
    const auto infiniteResult = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h,
                                                                            std::numeric_limits<float>::infinity());

    EXPECT_FALSE(nanResult.matched);
    EXPECT_FALSE(infiniteResult.matched);
}

TEST(GoldenImageTest_CompareWithGolden_NonFiniteConfigFailsClosed)
{
    const std::filesystem::path goldenDirectory =
        std::filesystem::temp_directory_path() / "sparkengine-golden-image-test-nonfinite-config";
    std::error_code ec;
    std::filesystem::remove_all(goldenDirectory, ec);
    std::filesystem::create_directories(goldenDirectory, ec);
    if (ec)
    {
        EXPECT_TRUE(false);
        return;
    }

    const std::vector<uint8_t> golden = {0, 0, 0, 255};
    const std::vector<uint8_t> actual = {255, 0, 0, 255};
    const auto goldenPath = goldenDirectory / "scene.png";
    if (!Spark::GoldenImageTestRunner::SavePNG(goldenPath.string(), golden.data(), 1, 1))
    {
        EXPECT_TRUE(false);
        std::filesystem::remove_all(goldenDirectory, ec);
        return;
    }

    auto& runner = Spark::GoldenImageTestRunner::GetInstance();
    Spark::GoldenImageConfig config;
    config.goldenImageDir = goldenDirectory.string();
    config.outputDir = (goldenDirectory / "output").string();

    config.perPixelThreshold = std::numeric_limits<float>::quiet_NaN();
    runner.Initialize(config);
    runner.SetCapture(std::make_unique<FixedCapture>(actual));
    const auto nanThresholdResult = runner.CompareWithGolden("scene");
    EXPECT_FALSE(nanThresholdResult.matched);

    config.perPixelThreshold = 0.0f;
    config.tolerancePercent = std::numeric_limits<float>::infinity();
    runner.Initialize(config);
    runner.SetCapture(std::make_unique<FixedCapture>(actual));
    const auto infiniteToleranceResult = runner.CompareWithGolden("scene");
    EXPECT_FALSE(infiniteToleranceResult.matched);

    runner.Shutdown();
    std::filesystem::remove_all(goldenDirectory, ec);
}

// ============================================================================
// CompareImages — null/zero inputs
// ============================================================================

TEST(GoldenImageTest_CompareImages_NullData)
{
    auto result = Spark::GoldenImageTestRunner::CompareImages(nullptr, nullptr, 10, 10, 10.0f);

    EXPECT_FALSE(result.matched);
}

TEST(GoldenImageTest_CompareImages_ZeroDimensions)
{
    std::vector<uint8_t> data(16, 0);
    auto result = Spark::GoldenImageTestRunner::CompareImages(data.data(), data.data(), 0, 0, 10.0f);

    EXPECT_FALSE(result.matched);
}

// ============================================================================
// HasRegressions
// ============================================================================

TEST(GoldenImageTest_HasRegressions_AllMatch)
{
    std::vector<Spark::ImageComparisonResult> results;
    Spark::ImageComparisonResult r1;
    r1.matched = true;
    Spark::ImageComparisonResult r2;
    r2.matched = true;
    results.push_back(r1);
    results.push_back(r2);

    EXPECT_FALSE(Spark::GoldenImageTestRunner::HasRegressions(results));
}

TEST(GoldenImageTest_HasRegressions_OneFailure)
{
    std::vector<Spark::ImageComparisonResult> results;
    Spark::ImageComparisonResult r1;
    r1.matched = true;
    Spark::ImageComparisonResult r2;
    r2.matched = false;
    results.push_back(r1);
    results.push_back(r2);

    EXPECT_TRUE(Spark::GoldenImageTestRunner::HasRegressions(results));
}

TEST(GoldenImageTest_HasRegressions_EmptyResultsFailClosed)
{
    std::vector<Spark::ImageComparisonResult> results;
    EXPECT_TRUE(Spark::GoldenImageTestRunner::HasRegressions(results));
}

TEST(GoldenImageTest_RunAllComparisons_EmptyDirectoryFailsClosed)
{
    const std::filesystem::path goldenDirectory =
        std::filesystem::temp_directory_path() / "sparkengine-golden-image-test-empty";
    std::error_code ec;
    std::filesystem::remove_all(goldenDirectory, ec);
    std::filesystem::create_directories(goldenDirectory, ec);

    Spark::GoldenImageConfig config;
    config.goldenImageDir = goldenDirectory.string();
    config.outputDir = (goldenDirectory / "output").string();

    auto& runner = Spark::GoldenImageTestRunner::GetInstance();
    runner.Initialize(config);
    const auto results = runner.RunAllComparisons();

    EXPECT_EQ(results.size(), 1u);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::HasRegressions(results));
    if (!results.empty())
    {
        EXPECT_FALSE(results.front().matched);
    }

    runner.Shutdown();
    std::filesystem::remove_all(goldenDirectory, ec);
}

// ============================================================================
// GoldenImageConfig default values
// ============================================================================

TEST(GoldenImageTest_Config_Defaults)
{
    Spark::GoldenImageConfig config;
    EXPECT_NEAR(config.tolerancePercent, 0.5f, 0.001f);
    EXPECT_NEAR(config.perPixelThreshold, 10.0f, 0.001f);
    EXPECT_EQ(config.maxDiffsToReport, 100u);
    EXPECT_TRUE(config.goldenImageDir.empty());
    EXPECT_TRUE(config.outputDir.empty());
}

// ============================================================================
// ImageComparisonResult default values
// ============================================================================

TEST(GoldenImageTest_Result_Defaults)
{
    Spark::ImageComparisonResult result;
    EXPECT_TRUE(result.matched);
    EXPECT_EQ(result.totalPixels, 0u);
    EXPECT_EQ(result.differentPixels, 0u);
    EXPECT_NEAR(result.percentDifferent, 0.0f, 0.001f);
    EXPECT_NEAR(result.maxPixelDistance, 0.0f, 0.001f);
}

// ============================================================================
// CompareImages — averagePixelDistance
// ============================================================================

TEST(GoldenImageTest_CompareImages_AverageDistance)
{
    constexpr uint32_t w = 2;
    constexpr uint32_t h = 1;
    std::vector<uint8_t> golden = {0, 0, 0, 255, 0, 0, 0, 255};
    std::vector<uint8_t> actual = {0, 0, 0, 255, 0, 0, 0, 255};

    auto result = Spark::GoldenImageTestRunner::CompareImages(golden.data(), actual.data(), w, h, 10.0f);

    EXPECT_NEAR(result.averagePixelDistance, 0.0f, 0.001f);
}
