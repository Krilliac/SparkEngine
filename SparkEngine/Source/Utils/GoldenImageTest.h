/**
 * @file GoldenImageTest.h
 * @brief Golden image / screenshot regression testing framework
 * @author Spark Engine Team
 * @date 2026
 *
 * Captures framebuffer screenshots, compares them pixel-by-pixel against
 * stored golden reference images, and reports visual regressions. Baselines
 * are real PNG files (GoldenImagePng.h, vendored miniz). Every comparison is
 * governed by a reviewed-threshold manifest (`<goldenImageDir>/manifest.json`)
 * that pins, per scene and backend row, the thresholds, the reviewer, and the
 * SHA-256 of the committed baseline. Comparisons fail closed: a missing or
 * malformed manifest, a missing entry, a missing baseline, or a baseline whose
 * hash differs from the reviewed one is a failure, never a skip.
 *
 * ## Architecture
 * ```
 * GoldenImageTestRunner (singleton)
 *   ├── m_config             (directories, backend row)
 *   ├── m_capture            (IGoldenImageCapture for framebuffer readback)
 *   ├── CaptureGolden()      (write <row>/<scene>.png for review)
 *   ├── CompareWithGolden()  (manifest + hash gate, then pixel diff)
 *   └── RunAllComparisons()  (every manifest entry for the backend row)
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
 *   cfg.backendRow     = "vulkan-lavapipe";
 *   runner.Initialize(cfg);
 *   runner.SetCapture(std::make_unique<MyFramebufferCapture>());
 *
 *   auto result = runner.CompareWithGolden("MainMenu");
 *   if (!result.matched)
 *       LOG_ERROR("Regression: {} ({}% pixels differ)", result.failureReason, result.percentDifferent);
 * @endcode
 *
 * @see Tests/GoldenImages/README.md, BenchmarkFramework.h
 */

#pragma once

#include "../Core/FileIntegrity.h"
#include "GoldenImageManifest.h"
#include "GoldenImagePng.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
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
        float perPixelThreshold = 0.f;    ///< Reviewed per-pixel threshold applied (from the manifest).
        float tolerancePercent = 0.f;     ///< Reviewed differing-pixel tolerance applied (from the manifest).
        std::string failureReason;        ///< Why the comparison failed closed; empty on a pixel-level verdict.
        std::string diffImagePath;        ///< Path to the generated diff image.
        std::vector<PixelDiff> diffs;     ///< First N differing pixels (capped).
    };

    /**
     * @brief Configuration for the golden image test runner.
     *
     * Thresholds are deliberately absent: they come only from the reviewed
     * manifest entry for the scene and backend row being compared.
     */
    struct GoldenImageConfig
    {
        std::string goldenImageDir;      ///< Directory containing manifest.json and <row>/<scene>.png baselines.
        std::string outputDir;           ///< Directory for captured / diff images.
        std::string backendRow;          ///< Backend row under test (see GoldenManifest::IsKnownBackendRow).
        uint32_t maxDiffsToReport = 100; ///< Cap on PixelDiff entries stored in results.
    };

    /**
     * @brief Colour statistics used to reject blank (uniform) frames.
     */
    struct FrameContent
    {
        size_t distinctColors = 0;     ///< Number of distinct RGB colours.
        double dominantFraction = 1.0; ///< Share of pixels equal to the most common colour.
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
         * @param config Directories, backend row, and limits.
         */
        void Initialize(const GoldenImageConfig& config)
        {
            m_config = config;
            m_capture.reset();

            std::error_code ec;
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
         * @brief Capture the current framebuffer and write it as <backendRow>/<scene>.png.
         *
         * The new file does not become a baseline until a reviewer records its
         * SHA-256 and thresholds in the manifest; until then comparisons fail
         * on the hash check.
         *
         * @param sceneName Scene/test name used for the file name.
         * @return True when a PNG was written.
         */
        bool CaptureGolden(std::string_view sceneName)
        {
            if (!m_capture || !GoldenManifest::IsKnownBackendRow(m_config.backendRow) ||
                !GoldenManifest::IsValidSceneId(sceneName))
            {
                return false;
            }

            constexpr uint32_t kDefaultWidth = 1920;
            constexpr uint32_t kDefaultHeight = 1080;

            auto pixels = m_capture->CaptureFramebuffer(kDefaultWidth, kDefaultHeight);
            if (pixels.size() != static_cast<size_t>(kDefaultWidth) * kDefaultHeight * 4)
            {
                return false;
            }

            const std::filesystem::path path = GoldenPath(sceneName);
            std::error_code ec;
            std::filesystem::create_directories(path.parent_path(), ec);
            return SavePNG(path.string(), pixels.data(), kDefaultWidth, kDefaultHeight);
        }

        /**
         * @brief Compare the current framebuffer against the reviewed golden image.
         *
         * Fails closed (matched == false, failureReason set) on an unknown
         * backend row, an invalid scene id, a missing/invalid manifest, a
         * missing manifest entry, a baseline hash mismatch, an undecodable
         * baseline, a missing capture, or a size mismatch.
         *
         * @param sceneName Scene/test name identifying the golden reference.
         * @return Comparison result with match status and diff details.
         */
        [[nodiscard]] ImageComparisonResult CompareWithGolden(std::string_view sceneName)
        {
            ImageComparisonResult result;
            result.sceneName = std::string(sceneName);

            auto failClosed = [&result](std::string reason)
            {
                result.matched = false;
                result.failureReason = std::move(reason);
                return result;
            };

            if (!GoldenManifest::IsKnownBackendRow(m_config.backendRow))
            {
                return failClosed("unknown backend row '" + m_config.backendRow + "'");
            }
            if (!GoldenManifest::IsValidSceneId(sceneName))
            {
                return failClosed("invalid scene id");
            }

            std::vector<GoldenManifestEntry> entries;
            std::string manifestError;
            if (!GoldenManifest::Load(ManifestPath(), entries, manifestError))
            {
                return failClosed("manifest: " + manifestError);
            }

            const auto entryIt =
                std::find_if(entries.begin(), entries.end(), [&](const GoldenManifestEntry& entry)
                             { return entry.scene == sceneName && entry.backendRow == m_config.backendRow; });
            if (entryIt == entries.end())
            {
                return failClosed("no manifest entry for scene '" + result.sceneName + "' on row '" +
                                  m_config.backendRow + "'");
            }
            const GoldenManifestEntry& entry = *entryIt;
            result.perPixelThreshold = entry.perPixelThreshold;
            result.tolerancePercent = entry.tolerancePercent;

            const std::filesystem::path goldenPath = GoldenPath(sceneName);
            std::string actualHash;
            std::string hashError;
            if (!FileIntegrity::ComputeSha256(goldenPath, actualHash, hashError))
            {
                return failClosed("baseline unreadable: " + goldenPath.string() + " (" + hashError + ")");
            }
            if (actualHash != entry.baselineSha256)
            {
                return failClosed("baseline SHA-256 " + actualHash + " does not match reviewed " +
                                  entry.baselineSha256);
            }

            uint32_t goldenW = 0;
            uint32_t goldenH = 0;
            const auto goldenPixels = LoadPNG(goldenPath.string(), goldenW, goldenH);
            if (goldenPixels.empty())
            {
                return failClosed("baseline is not a decodable PNG: " + goldenPath.string());
            }

            if (!m_capture)
            {
                return failClosed("no framebuffer capture set");
            }

            const auto actualPixels = m_capture->CaptureFramebuffer(goldenW, goldenH);
            if (actualPixels.size() != goldenPixels.size())
            {
                result.totalPixels = goldenW * goldenH;
                result.differentPixels = result.totalPixels;
                result.percentDifferent = 100.f;
                return failClosed("capture size does not match the baseline");
            }

            ImageComparisonResult compared =
                CompareImages(goldenPixels.data(), actualPixels.data(), goldenW, goldenH, entry.perPixelThreshold);
            compared.sceneName = result.sceneName;
            compared.perPixelThreshold = entry.perPixelThreshold;
            compared.tolerancePercent = entry.tolerancePercent;
            compared.matched = (compared.percentDifferent <= entry.tolerancePercent);

            if (compared.diffs.size() > m_config.maxDiffsToReport)
            {
                compared.diffs.resize(m_config.maxDiffsToReport);
            }

            // Keep the actual frame and a diff visualization for review.
            if (!compared.matched)
            {
                const std::string stem = m_config.outputDir + "/" + m_config.backendRow + "_" + compared.sceneName;
                compared.diffImagePath = stem + "_diff.png";
                auto diffImage = GenerateDiffImage(goldenPixels.data(), actualPixels.data(), goldenW, goldenH,
                                                   entry.perPixelThreshold);
                SavePNG(compared.diffImagePath, diffImage.data(), goldenW, goldenH);
                SavePNG(stem + ".png", actualPixels.data(), goldenW, goldenH);
            }

            return compared;
        }

        /**
         * @brief Compare every manifest entry for the configured backend row.
         * @return One result per entry, or one failed result when the manifest
         *         is unusable or lists nothing for the row.
         */
        [[nodiscard]] std::vector<ImageComparisonResult> RunAllComparisons()
        {
            std::vector<ImageComparisonResult> results;
            auto names = GetGoldenImageNames();
            if (names.empty())
            {
                ImageComparisonResult noEvidence;
                noEvidence.sceneName = "<no-golden-images>";
                noEvidence.matched = false;
                noEvidence.failureReason = "no reviewed manifest entries for row '" + m_config.backendRow + "'";
                results.push_back(noEvidence);
                return results;
            }

            results.reserve(names.size());
            for (const auto& name : names)
            {
                results.push_back(CompareWithGolden(name));
            }
            return results;
        }

        /**
         * @brief Check if comparisons are missing or any result contains a regression.
         * @param results Comparison results to inspect.
         * @return True when no comparisons ran or at least one result did not match.
         */
        [[nodiscard]] static bool HasRegressions(const std::vector<ImageComparisonResult>& results)
        {
            return results.empty() || std::any_of(results.begin(), results.end(),
                                                  [](const ImageComparisonResult& r) { return !r.matched; });
        }

        /**
         * @brief Overwrite <backendRow>/<scene>.png with the current framebuffer.
         * @param sceneName Scene/test name to update.
         * @return True when a PNG was written.
         */
        bool UpdateGolden(std::string_view sceneName) { return CaptureGolden(sceneName); }

        /**
         * @brief Scene ids the manifest lists for the configured backend row.
         * @return Sorted scene ids, or empty when the manifest is unusable.
         */
        [[nodiscard]] std::vector<std::string> GetGoldenImageNames() const
        {
            std::vector<std::string> names;
            std::vector<GoldenManifestEntry> entries;
            std::string error;
            if (!GoldenManifest::Load(ManifestPath(), entries, error))
            {
                return names;
            }

            for (const auto& entry : entries)
            {
                if (entry.backendRow == m_config.backendRow)
                {
                    names.push_back(entry.scene);
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
            result.perPixelThreshold = tolerance;

            if (!std::isfinite(tolerance) || !golden || !actual || w == 0 || h == 0)
            {
                result.matched = false;
                result.failureReason = "invalid comparison input";
                return result;
            }

            double totalDistance = 0.0;
            float maxDist = 0.f;

            for (uint32_t y = 0; y < h; ++y)
            {
                for (uint32_t x = 0; x < w; ++x)
                {
                    const size_t idx = (static_cast<size_t>(y) * w + x) * 4;
                    const float dist = PixelDistance(golden + idx, actual + idx);

                    totalDistance += static_cast<double>(dist);
                    maxDist = (std::max)(maxDist, dist);

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
            result.averagePixelDistance = static_cast<float>(totalDistance / result.totalPixels);
            result.percentDifferent =
                (static_cast<float>(result.differentPixels) / static_cast<float>(result.totalPixels)) * 100.f;
            result.matched = (result.differentPixels == 0);
            return result;
        }

        /**
         * @brief Colour statistics of a tightly packed RGBA8 frame.
         * @param rgba Frame pixels (alpha ignored).
         * @return Distinct colour count and dominant-colour share.
         */
        [[nodiscard]] static FrameContent AnalyzeFrame(const std::vector<uint8_t>& rgba)
        {
            FrameContent content;
            const size_t pixelCount = rgba.size() / 4;
            if (pixelCount == 0)
            {
                return content;
            }

            std::unordered_map<uint32_t, size_t> histogram;
            size_t dominant = 0;
            for (size_t i = 0; i < pixelCount; ++i)
            {
                const uint32_t packed =
                    (uint32_t(rgba[i * 4 + 0]) << 16) | (uint32_t(rgba[i * 4 + 1]) << 8) | uint32_t(rgba[i * 4 + 2]);
                dominant = (std::max)(dominant, ++histogram[packed]);
            }

            content.distinctColors = histogram.size();
            content.dominantFraction = double(dominant) / double(pixelCount);
            return content;
        }

        /**
         * @brief A frame has rendered content only if it has at least two colours
         *        and no single colour covers more than maxDominantFraction of it.
         *        A uniform (blank) frame always fails.
         */
        [[nodiscard]] static bool FrameHasRenderedContent(const std::vector<uint8_t>& rgba, double maxDominantFraction)
        {
            const FrameContent content = AnalyzeFrame(rgba);
            return content.distinctColors >= 2 && content.dominantFraction <= maxDominantFraction;
        }

        /**
         * @brief Encode RGBA pixel data as a PNG file.
         * @param path Output file path.
         * @param data RGBA pixel data (w * h * 4 bytes, top row first).
         * @param w    Image width.
         * @param h    Image height.
         * @return True on success.
         */
        static bool SavePNG(std::string_view path, const uint8_t* data, uint32_t w, uint32_t h)
        {
            return GoldenPng::WriteRGBA(std::filesystem::path(path), data, w, h);
        }

        /**
         * @brief Decode an 8-bit RGB/RGBA non-interlaced PNG to RGBA8. Anything else is rejected.
         * @param path Input file path.
         * @param w    [out] Image width.
         * @param h    [out] Image height.
         * @return RGBA pixel data, or empty vector on failure.
         */
        [[nodiscard]] static std::vector<uint8_t> LoadPNG(std::string_view path, uint32_t& w, uint32_t& h)
        {
            return GoldenPng::ReadRGBA(std::filesystem::path(path), w, h);
        }

        // -----------------------------------------------------------------
        // Console
        // -----------------------------------------------------------------

        /** @brief Human-readable status for the engine console. */
        [[nodiscard]] std::string Console_GetStatus() const
        {
            std::ostringstream oss;
            oss << "[GoldenImageTest] goldenDir=" << m_config.goldenImageDir << ", outputDir=" << m_config.outputDir
                << ", row=" << (m_config.backendRow.empty() ? "<unset>" : m_config.backendRow)
                << ", capture=" << (m_capture ? "set" : "none");
            return oss.str();
        }

      private:
        GoldenImageTestRunner() = default;
        GoldenImageTestRunner(const GoldenImageTestRunner&) = delete;
        GoldenImageTestRunner& operator=(const GoldenImageTestRunner&) = delete;

        /** @brief Path of the reviewed-threshold manifest. */
        [[nodiscard]] std::filesystem::path ManifestPath() const
        {
            return std::filesystem::path(m_config.goldenImageDir) / "manifest.json";
        }

        /** @brief Baseline path: <goldenImageDir>/<backendRow>/<scene>.png. */
        [[nodiscard]] std::filesystem::path GoldenPath(std::string_view sceneName) const
        {
            return std::filesystem::path(m_config.goldenImageDir) / m_config.backendRow /
                   (std::string(sceneName) + ".png");
        }

        /** @brief Euclidean RGB distance between two RGBA pixels. */
        [[nodiscard]] static float PixelDistance(const uint8_t* a, const uint8_t* b)
        {
            const float dr = static_cast<float>(a[0]) - static_cast<float>(b[0]);
            const float dg = static_cast<float>(a[1]) - static_cast<float>(b[1]);
            const float db = static_cast<float>(a[2]) - static_cast<float>(b[2]);
            return std::sqrt(dr * dr + dg * dg + db * db);
        }

        /**
         * @brief Generate a diff visualization image.
         * @return RGBA diff image (red = different, dim green = matching).
         */
        [[nodiscard]] static std::vector<uint8_t> GenerateDiffImage(const uint8_t* golden, const uint8_t* actual,
                                                                    uint32_t w, uint32_t h, float threshold)
        {
            const size_t pixelCount = static_cast<size_t>(w) * h;
            std::vector<uint8_t> diff(pixelCount * 4);

            for (size_t i = 0; i < pixelCount; ++i)
            {
                const size_t idx = i * 4;
                const float dist = PixelDistance(golden + idx, actual + idx);
                if (dist > threshold)
                {
                    // Red intensity proportional to distance; 441.7 = sqrt(3*255^2).
                    diff[idx + 0] = static_cast<uint8_t>((std::min)(255.f, dist * (255.f / 441.7f)));
                    diff[idx + 1] = 0;
                    diff[idx + 2] = 0;
                }
                else
                {
                    diff[idx + 0] = 0;
                    diff[idx + 1] = 32;
                    diff[idx + 2] = 0;
                }
                diff[idx + 3] = 255;
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
