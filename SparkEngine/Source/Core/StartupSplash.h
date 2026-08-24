/**
 * @file StartupSplash.h
 * @brief Codec-free, cross-platform startup splash configuration and renderer.
 */

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Spark
{
    struct StartupSplashRect
    {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    struct StartupSplashBitmap
    {
        int width = 0;
        int height = 0;
        /// Pixels are packed as 0xAARRGGBB.
        std::vector<uint32_t> pixels;

        [[nodiscard]] bool IsValid() const
        {
            return width > 0 && height > 0 && pixels.size() == static_cast<size_t>(width) * height;
        }
    };

    struct StartupSplashConfig
    {
        bool enabled = true;
        bool muted = false;
        double durationSeconds = 2.8;
        uint32_t accentRgb = 0xFF7818;
        std::filesystem::path imagePath;
        std::filesystem::path audioPath;
    };

    struct StartupSplashContext
    {
        std::vector<std::string> arguments;
        bool headless = false;
        bool automatedTest = false;
        std::filesystem::path projectRoot;
        std::filesystem::path executableDirectory;
    };

    /// Resolve defaults, Config/StartupSplash.ini, then command-line overrides.
    /// [startup/main thread] Internal, non-hot-reloadable process-start API.
    [[nodiscard]] StartupSplashConfig ResolveStartupSplashConfig(const StartupSplashContext& context);

    /// Central policy used by both platform entry points. [startup/main thread]
    [[nodiscard]] bool ShouldShowStartupSplash(const StartupSplashContext& context, const StartupSplashConfig& config);

    /// Calculate a centered aspect-preserving destination rectangle.
    [[nodiscard]] StartupSplashRect CalculateStartupSplashLetterbox(int destinationWidth, int destinationHeight,
                                                                    int contentWidth = 1920, int contentHeight = 1080);

    /// Load an uncompressed Windows BMP (24-bit or 32-bit). Invalid files return an empty bitmap.
    /// [startup/main thread] Accepts only the deliberately small BMP subset documented by the runtime.
    [[nodiscard]] StartupSplashBitmap LoadStartupSplashBmp(const std::filesystem::path& path);

    /**
     * Render one deterministic 16:9 frame into a 0xAARRGGBB buffer.
     * An optional project bitmap replaces the built-in SparkEngine wordmark
     * while retaining the ignition, scan, hold, and fade choreography.
     */
    void RenderStartupSplashFrame(const StartupSplashConfig& config, double elapsedSeconds, int width, int height,
                                  std::vector<uint32_t>& pixels, const StartupSplashBitmap* projectBitmap = nullptr);

    /**
     * Resolve and play the platform splash. Returns false only when policy
     * skipped it or the platform could not create a presentation surface;
     * engine startup should continue in either case.
     */
    /// [startup/main thread] Stack-owned; creates no persistent engine resources or worker threads.
    bool PlayStartupSplash(const StartupSplashContext& context);
} // namespace Spark
