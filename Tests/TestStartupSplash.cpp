/**
 * @file TestStartupSplash.cpp
 * @brief Policy, configuration, layout, BMP, and CPU-renderer tests.
 */

#include "TestFramework.h"
#include "Core/StartupSplash.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace
{
    std::filesystem::path UniqueSplashTestDirectory()
    {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() / ("spark-startup-splash-" + std::to_string(suffix));
    }

    void WriteTinyBmp(const std::filesystem::path& path)
    {
        // 2x2 bottom-up BGR24. Each four-byte-aligned row is 8 bytes.
        std::array<unsigned char, 70> bmp{};
        bmp[0] = 'B';
        bmp[1] = 'M';
        bmp[2] = 70;
        bmp[10] = 54;
        bmp[14] = 40;
        bmp[18] = 2;
        bmp[22] = 2;
        bmp[26] = 1;
        bmp[28] = 24;
        bmp[34] = 16;
        // Bottom row: blue, white. Top row: red, green.
        const std::array<unsigned char, 16> pixels = {255, 0, 0, 255, 255, 255, 0, 0, 0, 0, 255, 0, 255, 0, 0, 0};
        std::copy(pixels.begin(), pixels.end(), bmp.begin() + 54);
        std::ofstream stream(path, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bmp.data()), bmp.size());
    }
} // namespace

TEST(StartupSplash_LetterboxPreservesSixteenByNine)
{
    const auto rect = Spark::CalculateStartupSplashLetterbox(1024, 768);
    EXPECT_EQ(rect.x, 0);
    EXPECT_EQ(rect.y, 96);
    EXPECT_EQ(rect.width, 1024);
    EXPECT_EQ(rect.height, 576);

    const auto portrait = Spark::CalculateStartupSplashLetterbox(600, 900);
    EXPECT_EQ(portrait.width, 600);
    EXPECT_EQ(portrait.height, 337);
    EXPECT_EQ(portrait.y, 281);
}

TEST(StartupSplash_PolicySkipsServerAndAutomation)
{
    Spark::StartupSplashConfig config;
    Spark::StartupSplashContext context;
    EXPECT_TRUE(Spark::ShouldShowStartupSplash(context, config));

    context.headless = true;
    EXPECT_FALSE(Spark::ShouldShowStartupSplash(context, config));
    context.headless = false;
    context.automatedTest = true;
    EXPECT_FALSE(Spark::ShouldShowStartupSplash(context, config));
    context.automatedTest = false;
    context.arguments = {"SparkEngine", "--test-seconds", "1"};
    EXPECT_FALSE(Spark::ShouldShowStartupSplash(context, config));
    context.arguments.clear();
    config.enabled = false;
    EXPECT_FALSE(Spark::ShouldShowStartupSplash(context, config));

    config.enabled = true;
    context.arguments = {"SparkEngine", "-dedicated"};
    EXPECT_FALSE(Spark::ShouldShowStartupSplash(context, config));
    context.arguments = {"SparkEngine", "-minimal-init"};
    EXPECT_FALSE(Spark::ShouldShowStartupSplash(context, config));
}

TEST(StartupSplash_ProjectConfigAndCommandLineOverrides)
{
    const auto root = UniqueSplashTestDirectory();
    std::filesystem::create_directories(root / "Config");
    {
        std::ofstream config(root / "Config/StartupSplash.ini");
        config << "enabled=true\n"
                  "mute=true\n"
                  "duration=4.5\n"
                  "accent=#12AB34\n"
                  "image=Assets/Branding/custom.bmp\n"
                  "audio=Assets/Audio/custom.wav\n";
    }

    Spark::StartupSplashContext context;
    context.projectRoot = root;
    context.arguments = {"SparkEngine", "-splash-duration", "1.25", "-no-splash"};
    const auto resolved = Spark::ResolveStartupSplashConfig(context);
    EXPECT_FALSE(resolved.enabled);
    EXPECT_TRUE(resolved.muted);
    EXPECT_NEAR(resolved.durationSeconds, 1.25, 0.001);
    EXPECT_EQ(resolved.accentRgb, static_cast<uint32_t>(0x12AB34));
    EXPECT_TRUE(resolved.imagePath == (root / "Assets/Branding/custom.bmp").lexically_normal());
    EXPECT_TRUE(resolved.audioPath == (root / "Assets/Audio/custom.wav").lexically_normal());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(StartupSplash_RejectsUnconfinedOverridesAndNonFiniteDurations)
{
    const auto root = UniqueSplashTestDirectory();
    std::filesystem::create_directories(root / "Config");
    std::filesystem::create_directories(root / "Assets/Branding");
    {
        std::ofstream config(root / "Config/StartupSplash.ini");
        config << "duration=nan\n"
                  "image=../outside.bmp\n"
                  "audio=C:/outside.wav\n";
    }

    Spark::StartupSplashContext context;
    context.projectRoot = root;
    context.arguments = {"SparkEngine", "-splash-duration", "inf"};
    const auto resolved = Spark::ResolveStartupSplashConfig(context);
    EXPECT_NEAR(resolved.durationSeconds, 2.8, 0.001);
    EXPECT_TRUE(resolved.imagePath.empty());
    EXPECT_TRUE(resolved.audioPath.empty());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(StartupSplash_BmpLoaderReadsOrientationAndChannels)
{
    const auto root = UniqueSplashTestDirectory();
    std::filesystem::create_directories(root);
    const auto bmpPath = root / "tiny.bmp";
    WriteTinyBmp(bmpPath);

    const auto bitmap = Spark::LoadStartupSplashBmp(bmpPath);
    EXPECT_TRUE(bitmap.IsValid());
    EXPECT_EQ(bitmap.width, 2);
    EXPECT_EQ(bitmap.height, 2);
    EXPECT_EQ(bitmap.pixels[0], static_cast<uint32_t>(0xFFFF0000));
    EXPECT_EQ(bitmap.pixels[1], static_cast<uint32_t>(0xFF00FF00));
    EXPECT_EQ(bitmap.pixels[2], static_cast<uint32_t>(0xFF0000FF));
    EXPECT_EQ(bitmap.pixels[3], static_cast<uint32_t>(0xFFFFFFFF));

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(StartupSplash_BmpLoaderRejectsTruncationAndOversizedDimensions)
{
    const auto root = UniqueSplashTestDirectory();
    std::filesystem::create_directories(root);

    const auto truncatedPath = root / "truncated.bmp";
    {
        std::ofstream stream(truncatedPath, std::ios::binary);
        stream << "BM";
    }
    EXPECT_FALSE(Spark::LoadStartupSplashBmp(truncatedPath).IsValid());

    const auto oversizedPath = root / "oversized.bmp";
    std::array<unsigned char, 54> header{};
    header[0] = 'B';
    header[1] = 'M';
    header[10] = 54;
    header[14] = 40;
    header[18] = 0x01;
    header[20] = 0x01; // 65,537 pixels wide.
    header[22] = 1;
    header[26] = 1;
    header[28] = 24;
    {
        std::ofstream stream(oversizedPath, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(header.data()), header.size());
    }
    EXPECT_FALSE(Spark::LoadStartupSplashBmp(oversizedPath).IsValid());

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

TEST(StartupSplash_RenderIsDeterministicAndFadesToBlack)
{
    Spark::StartupSplashConfig config;
    std::vector<uint32_t> first;
    std::vector<uint32_t> second;
    Spark::RenderStartupSplashFrame(config, 1.6, 320, 180, first);
    Spark::RenderStartupSplashFrame(config, 1.6, 320, 180, second);
    EXPECT_TRUE(first == second);
    EXPECT_EQ(first.size(), static_cast<size_t>(320 * 180));
    EXPECT_TRUE(std::any_of(first.begin(), first.end(), [](uint32_t pixel) { return pixel != 0xFF050608u; }));

    Spark::RenderStartupSplashFrame(config, config.durationSeconds, 320, 180, second);
    EXPECT_TRUE(std::all_of(second.begin(), second.end(), [](uint32_t pixel) { return pixel == 0xFF000000u; }));
}
