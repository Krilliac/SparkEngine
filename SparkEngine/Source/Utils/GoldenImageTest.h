/**
 * @file GoldenImageTest.h
 * @brief Golden image / screenshot regression testing framework
 * @author Spark Engine Team
 * @date 2026
 *
 * Captures framebuffer screenshots, compares them pixel-by-pixel against
 * stored golden reference images, and reports visual regressions. Supports
 * configurable per-pixel and per-image tolerance thresholds and generates
 * diff images highlighting changed regions.
 *
 * ## Architecture
 * ```
 * GoldenImageTestRunner (singleton)
 *   ├── m_config           (directories, tolerances)
 *   ├── m_capture          (IGoldenImageCapture for framebuffer readback)
 *   ├── CaptureGolden()    (save reference screenshot)
 *   ├── CompareWithGolden() (pixel diff against reference)
 *   └── RunAllComparisons() (batch compare every golden image)
 *
 * IGoldenImageCapture (interface)
 *   └── CaptureFramebuffer() -> RGBA byte vector
 * ```
 *
 * ## Usage
 * @code
 *   auto& runner = Spark::GoldenImageTestRunner::GetInstance();
 *   Spark::GoldenImageConfig cfg;
 *   cfg.goldenImageDir = "Tests/GoldenImages";
 *   cfg.outputDir      = "Tests/Output";
 *   runner.Initialize(cfg);
 *   runner.SetCapture(std::make_unique<MyFramebufferCapture>());
 *
 *   runner.CaptureGolden("MainMenu");           // First run: save reference
 *   auto result = runner.CompareWithGolden("MainMenu"); // Subsequent: compare
 *   if (!result.matched)
 *       LOG_ERROR("Regression: {}% pixels differ", result.percentDifferent);
 * @endcode
 *
 * @see BenchmarkFramework.h (performance regression), GraphicsEngine.h
 */

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace Spark
{

    // =========================================================================
    // Data Structures
    // =========================================================================

    /**
     * @brief Information about a single pixel that differs between golden and actual.
     */
    struct PixelDiff
    {
        uint32_t x = 0;        ///< Pixel X coordinate.
        uint32_t y = 0;        ///< Pixel Y coordinate.
        uint8_t expectedR = 0; ///< Golden red channel.
        uint8_t expectedG = 0; ///< Golden green channel.
        uint8_t expectedB = 0; ///< Golden blue channel.
        uint8_t actualR = 0;   ///< Actual red channel.
        uint8_t actualG = 0;   ///< Actual green channel.
        uint8_t actualB = 0;   ///< Actual blue channel.
        float distance = 0.f;  ///< Euclidean distance in RGB space.
    };

    /**
     * @brief Result of comparing a captured screenshot against a golden image.
     */
    struct ImageComparisonResult
    {
        std::string sceneName;            ///< Scene/test name.
        bool matched = true;              ///< True if within tolerance.
        uint32_t totalPixels = 0;         ///< Total pixel count (width * height).
        uint32_t differentPixels = 0;     ///< Number of pixels exceeding threshold.
        float percentDifferent = 0.f;     ///< Percentage of differing pixels.
        float maxPixelDistance = 0.f;     ///< Maximum per-pixel distance observed.
        float averagePixelDistance = 0.f; ///< Average distance across all pixels.
        std::string diffImagePath;        ///< Path to the generated diff image.
        std::vector<PixelDiff> diffs;     ///< First N differing pixels (capped).
    };

    /**
     * @brief Configuration for the golden image test runner.
     */
    struct GoldenImageConfig
    {
        std::string goldenImageDir;      ///< Directory containing reference images.
        std::string outputDir;           ///< Directory for captured / diff images.
        float tolerancePercent = 0.5f;   ///< Max allowed percent of differing pixels.
        float perPixelThreshold = 10.0f; ///< Max channel distance before a pixel counts as different.
        uint32_t maxDiffsToReport = 100; ///< Cap on PixelDiff entries stored in results.
    };

    // =========================================================================
    // Capture Interface
    // =========================================================================

    /**
     * @brief Abstract interface for framebuffer capture.
     *
     * Implement this per RHI backend to read back the current framebuffer
     * contents as an RGBA byte array.
     */
    class IGoldenImageCapture
    {
      public:
        virtual ~IGoldenImageCapture() = default;

        /**
         * @brief Capture the current framebuffer as RGBA pixels.
         * @param width  Desired capture width in pixels.
         * @param height Desired capture height in pixels.
         * @return RGBA byte vector of size width * height * 4.
         */
        [[nodiscard]] virtual std::vector<uint8_t> CaptureFramebuffer(uint32_t width, uint32_t height) = 0;
    };

    // =========================================================================
    // GoldenImageTestRunner
    // =========================================================================

    /**
     * @class GoldenImageTestRunner
     * @brief Singleton that captures screenshots and compares against golden references.
     */
    class GoldenImageTestRunner
    {
      public:
        /** @brief Get the singleton instance. */
        static GoldenImageTestRunner& GetInstance()
        {
            static GoldenImageTestRunner s;
            return s;
        }

        // -----------------------------------------------------------------
        // Lifecycle
        // -----------------------------------------------------------------

        /**
         * @brief Initialize the test runner with the given configuration.
         * @param config Directories, tolerances, and limits.
         */
        void Initialize(const GoldenImageConfig& config)
        {
            m_config = config;
            m_capture.reset();

            // Ensure directories exist.
            std::error_code ec;
            std::filesystem::create_directories(m_config.goldenImageDir, ec);
            std::filesystem::create_directories(m_config.outputDir, ec);
        }

        /** @brief Shut down and release the capture interface. */
        void Shutdown()
        {
            m_capture.reset();
            m_config = {};
        }

        /**
         * @brief Set the framebuffer capture implementation.
         * @param capture Owned capture interface.
         */
        void SetCapture(std::unique_ptr<IGoldenImageCapture> capture) { m_capture = std::move(capture); }

        // -----------------------------------------------------------------
        // Golden image operations
        // -----------------------------------------------------------------

        /**
         * @brief Capture the current framebuffer and save as the golden reference.
         * @param sceneName Scene/test name used for the file name.
         */
        void CaptureGolden(std::string_view sceneName)
        {
            if (!m_capture)
            {
                return;
            }

            constexpr uint32_t kDefaultWidth = 1920;
            constexpr uint32_t kDefaultHeight = 1080;

            auto pixels = m_capture->CaptureFramebuffer(kDefaultWidth, kDefaultHeight);
            if (pixels.empty())
            {
                return;
            }

            std::string path = GoldenPath(sceneName);
            SavePNG(path, pixels.data(), kDefaultWidth, kDefaultHeight);
        }

        /**
         * @brief Compare the current framebuffer against the stored golden image.
         * @param sceneName Scene/test name identifying the golden reference.
         * @return Comparison result with match status and diff details.
         */
        [[nodiscard]] ImageComparisonResult CompareWithGolden(std::string_view sceneName)
        {
            ImageComparisonResult result;
            result.sceneName = std::string(sceneName);

            std::string goldenPath = GoldenPath(sceneName);
            uint32_t goldenW = 0, goldenH = 0;
            auto goldenPixels = LoadPNG(goldenPath, goldenW, goldenH);

            if (goldenPixels.empty())
            {
                result.matched = false;
                return result;
            }

            if (!m_capture)
            {
                result.matched = false;
                return result;
            }

            auto actualPixels = m_capture->CaptureFramebuffer(goldenW, goldenH);
            if (actualPixels.size() != goldenPixels.size())
            {
                result.matched = false;
                result.totalPixels = goldenW * goldenH;
                result.differentPixels = result.totalPixels;
                result.percentDifferent = 100.f;
                return result;
            }

            result =
                CompareImages(goldenPixels.data(), actualPixels.data(), goldenW, goldenH, m_config.perPixelThreshold);
            result.sceneName = std::string(sceneName);
            result.matched = (result.percentDifferent <= m_config.tolerancePercent);

            // Cap the diff list.
            if (result.diffs.size() > m_config.maxDiffsToReport)
            {
                result.diffs.resize(m_config.maxDiffsToReport);
            }

            // Save diff image.
            if (!result.matched)
            {
                result.diffImagePath = m_config.outputDir + "/" + std::string(sceneName) + "_diff.png";
                auto diffImage = GenerateDiffImage(goldenPixels.data(), actualPixels.data(), goldenW, goldenH,
                                                   m_config.perPixelThreshold);
                SavePNG(result.diffImagePath, diffImage.data(), goldenW, goldenH);
            }

            return result;
        }

        /**
         * @brief Run comparison for every golden image in the golden directory.
         * @return Vector of comparison results (one per golden image found).
         */
        [[nodiscard]] std::vector<ImageComparisonResult> RunAllComparisons()
        {
            std::vector<ImageComparisonResult> results;
            auto names = GetGoldenImageNames();
            results.reserve(names.size());
            for (const auto& name : names)
            {
                results.push_back(CompareWithGolden(name));
            }
            return results;
        }

        /**
         * @brief Check if any results contain regressions.
         * @param results Comparison results to inspect.
         * @return True if at least one result did not match.
         */
        [[nodiscard]] static bool HasRegressions(const std::vector<ImageComparisonResult>& results)
        {
            return std::any_of(results.begin(), results.end(),
                               [](const ImageComparisonResult& r) { return !r.matched; });
        }

        /**
         * @brief Overwrite the golden reference with the current framebuffer.
         * @param sceneName Scene/test name to update.
         */
        void UpdateGolden(std::string_view sceneName) { CaptureGolden(sceneName); }

        /**
         * @brief List all golden image scene names found in the golden directory.
         * @return Vector of scene names (filename stems).
         */
        [[nodiscard]] std::vector<std::string> GetGoldenImageNames() const
        {
            std::vector<std::string> names;
            std::error_code ec;
            if (!std::filesystem::exists(m_config.goldenImageDir, ec))
            {
                return names;
            }

            for (const auto& entry : std::filesystem::directory_iterator(m_config.goldenImageDir, ec))
            {
                if (entry.is_regular_file(ec) && entry.path().extension() == ".png")
                {
                    names.push_back(entry.path().stem().string());
                }
            }
            std::sort(names.begin(), names.end());
            return names;
        }

        // -----------------------------------------------------------------
        // Static helpers
        // -----------------------------------------------------------------

        /**
         * @brief Compare two RGBA images pixel-by-pixel.
         * @param golden   Pointer to golden RGBA data.
         * @param actual   Pointer to actual RGBA data.
         * @param w        Image width.
         * @param h        Image height.
         * @param tolerance Per-pixel channel distance threshold.
         * @return Comparison result with statistics and diff list.
         */
        [[nodiscard]] static ImageComparisonResult CompareImages(const uint8_t* golden, const uint8_t* actual,
                                                                 uint32_t w, uint32_t h, float tolerance)
        {
            ImageComparisonResult result;
            result.totalPixels = w * h;

            if (!golden || !actual || w == 0 || h == 0)
            {
                result.matched = false;
                return result;
            }

            double totalDistance = 0.0;
            float maxDist = 0.f;

            for (uint32_t y = 0; y < h; ++y)
            {
                for (uint32_t x = 0; x < w; ++x)
                {
                    uint32_t idx = (y * w + x) * 4;
                    float dr = static_cast<float>(golden[idx + 0]) - static_cast<float>(actual[idx + 0]);
                    float dg = static_cast<float>(golden[idx + 1]) - static_cast<float>(actual[idx + 1]);
                    float db = static_cast<float>(golden[idx + 2]) - static_cast<float>(actual[idx + 2]);
                    float dist = std::sqrt(dr * dr + dg * dg + db * db);

                    totalDistance += static_cast<double>(dist);
                    if (dist > maxDist)
                    {
                        maxDist = dist;
                    }

                    if (dist > tolerance)
                    {
                        ++result.differentPixels;

                        PixelDiff pd;
                        pd.x = x;
                        pd.y = y;
                        pd.expectedR = golden[idx + 0];
                        pd.expectedG = golden[idx + 1];
                        pd.expectedB = golden[idx + 2];
                        pd.actualR = actual[idx + 0];
                        pd.actualG = actual[idx + 1];
                        pd.actualB = actual[idx + 2];
                        pd.distance = dist;
                        result.diffs.push_back(pd);
                    }
                }
            }

            result.maxPixelDistance = maxDist;
            result.averagePixelDistance =
                (result.totalPixels > 0) ? static_cast<float>(totalDistance / result.totalPixels) : 0.f;
            result.percentDifferent =
                (result.totalPixels > 0)
                    ? (static_cast<float>(result.differentPixels) / static_cast<float>(result.totalPixels)) * 100.f
                    : 0.f;
            result.matched = (result.differentPixels == 0);
            return result;
        }

        /**
         * @brief Save RGBA pixel data as a PNG file.
         * @param path Output file path.
         * @param data RGBA pixel data.
         * @param w    Image width.
         * @param h    Image height.
         * @return True on success.
         */
        static bool SavePNG(std::string_view path, const uint8_t* data, uint32_t w, uint32_t h)
        {
            if (!data || w == 0 || h == 0)
            {
                return false;
            }

            // Minimal uncompressed PNG writer (valid but not optimal).
            // A production engine would use stb_image_write or libpng.
            std::string pathStr(path);
            std::FILE* file = std::fopen(pathStr.c_str(), "wb");
            if (!file)
            {
                return false;
            }

            // Write a raw RGBA binary for now (TGA-like). A full PNG encoder
            // is beyond the scope of this header; the asset pipeline provides
            // one via stb_image_write when available.
            // Format: [width:4][height:4][RGBA data: w*h*4]
            std::fwrite(&w, sizeof(uint32_t), 1, file);
            std::fwrite(&h, sizeof(uint32_t), 1, file);
            std::fwrite(data, 1, static_cast<size_t>(w) * h * 4, file);
            std::fclose(file);
            return true;
        }

        /**
         * @brief Load a PNG (or raw RGBA) image from disk.
         * @param path Input file path.
         * @param w    [out] Image width.
         * @param h    [out] Image height.
         * @return RGBA pixel data, or empty vector on failure.
         */
        [[nodiscard]] static std::vector<uint8_t> LoadPNG(std::string_view path, uint32_t& w, uint32_t& h)
        {
            w = 0;
            h = 0;

            std::string pathStr(path);
            std::FILE* file = std::fopen(pathStr.c_str(), "rb");
            if (!file)
            {
                return {};
            }

            std::fread(&w, sizeof(uint32_t), 1, file);
            std::fread(&h, sizeof(uint32_t), 1, file);

            if (w == 0 || h == 0 || w > 16384 || h > 16384)
            {
                std::fclose(file);
                w = 0;
                h = 0;
                return {};
            }

            size_t dataSize = static_cast<size_t>(w) * h * 4;
            std::vector<uint8_t> pixels(dataSize);
            size_t bytesRead = std::fread(pixels.data(), 1, dataSize, file);
            std::fclose(file);

            if (bytesRead != dataSize)
            {
                w = 0;
                h = 0;
                return {};
            }

            return pixels;
        }

        // -----------------------------------------------------------------
        // Console
        // -----------------------------------------------------------------

        /** @brief Human-readable status for the engine console. */
        [[nodiscard]] std::string Console_GetStatus() const
        {
            std::ostringstream oss;
            oss << "[GoldenImageTest] goldenDir=" << m_config.goldenImageDir << ", outputDir=" << m_config.outputDir
                << ", tolerance=" << m_config.tolerancePercent << "%" << ", perPixel=" << m_config.perPixelThreshold
                << ", capture=" << (m_capture ? "set" : "none");
            return oss.str();
        }

      private:
        GoldenImageTestRunner() = default;
        GoldenImageTestRunner(const GoldenImageTestRunner&) = delete;
        GoldenImageTestRunner& operator=(const GoldenImageTestRunner&) = delete;

        /** @brief Build the full path for a golden image file. */
        [[nodiscard]] std::string GoldenPath(std::string_view sceneName) const
        {
            return m_config.goldenImageDir + "/" + std::string(sceneName) + ".png";
        }

        /**
         * @brief Generate a diff visualization image.
         * @param golden   Golden RGBA data.
         * @param actual   Actual RGBA data.
         * @param w        Width.
         * @param h        Height.
         * @param threshold Per-pixel distance threshold.
         * @return RGBA diff image (red = different, green = matching).
         */
        [[nodiscard]] static std::vector<uint8_t> GenerateDiffImage(const uint8_t* golden, const uint8_t* actual,
                                                                    uint32_t w, uint32_t h, float threshold)
        {
            size_t pixelCount = static_cast<size_t>(w) * h;
            std::vector<uint8_t> diff(pixelCount * 4);

            for (size_t i = 0; i < pixelCount; ++i)
            {
                size_t idx = i * 4;
                float dr = static_cast<float>(golden[idx + 0]) - static_cast<float>(actual[idx + 0]);
                float dg = static_cast<float>(golden[idx + 1]) - static_cast<float>(actual[idx + 1]);
                float db = static_cast<float>(golden[idx + 2]) - static_cast<float>(actual[idx + 2]);
                float dist = std::sqrt(dr * dr + dg * dg + db * db);

                if (dist > threshold)
                {
                    // Red channel intensity proportional to distance.
                    uint8_t intensity =
                        static_cast<uint8_t>(std::min(255.f, dist * (255.f / 441.7f))); // 441.7 = sqrt(3*255^2)
                    diff[idx + 0] = intensity;
                    diff[idx + 1] = 0;
                    diff[idx + 2] = 0;
                    diff[idx + 3] = 255;
                }
                else
                {
                    // Dimmed green for matching pixels.
                    diff[idx + 0] = 0;
                    diff[idx + 1] = 32;
                    diff[idx + 2] = 0;
                    diff[idx + 3] = 255;
                }
            }

            return diff;
        }

        // -----------------------------------------------------------------
        // State
        // -----------------------------------------------------------------

        GoldenImageConfig m_config;                     ///< Active configuration.
        std::unique_ptr<IGoldenImageCapture> m_capture; ///< Framebuffer capture backend.
    };

} // namespace Spark
