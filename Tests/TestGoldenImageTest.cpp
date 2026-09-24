// TestGoldenImageTest.cpp - Tests for Spark::GoldenImageTestRunner and the reviewed-threshold manifest
#include "TestFramework.h"
#include "Core/FileIntegrity.h"
#include "Utils/GoldenImageManifest.h"
#include "Utils/GoldenImageTest.h"

#include <miniz.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
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

    constexpr uint32_t kSceneSize = 10;
    constexpr const char* kRow = "vulkan-lavapipe";

    /// Deterministic 10x10 RGBA gradient used as a baseline.
    std::vector<uint8_t> MakeGradient()
    {
        std::vector<uint8_t> pixels(kSceneSize * kSceneSize * 4);
        for (uint32_t i = 0; i < kSceneSize * kSceneSize; ++i)
        {
            pixels[i * 4 + 0] = static_cast<uint8_t>(i * 2);
            pixels[i * 4 + 1] = static_cast<uint8_t>(255 - i);
            pixels[i * 4 + 2] = static_cast<uint8_t>(i % 7 * 30);
            pixels[i * 4 + 3] = 255;
        }
        return pixels;
    }

    /// Scratch golden directory: <tmp>/<name>, recreated empty, removed on destruction.
    struct GoldenScratch
    {
        std::filesystem::path root;

        explicit GoldenScratch(const std::string& name)
            : root(std::filesystem::temp_directory_path() / ("sparkengine-golden-" + name))
        {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            std::filesystem::create_directories(root / kRow, ec);
        }

        ~GoldenScratch()
        {
            Spark::GoldenImageTestRunner::GetInstance().Shutdown();
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
        }

        /// Writes <row>/<scene>.png and returns its SHA-256 (empty on failure).
        std::string WriteBaseline(const std::string& scene, const std::vector<uint8_t>& pixels) const
        {
            const auto path = root / kRow / (scene + ".png");
            if (!Spark::GoldenImageTestRunner::SavePNG(path.string(), pixels.data(), kSceneSize, kSceneSize))
                return {};
            std::string digest;
            std::string error;
            return Spark::FileIntegrity::ComputeSha256(path, digest, error) ? digest : std::string{};
        }

        void WriteManifest(const std::string& text) const
        {
            std::ofstream(root / "manifest.json", std::ios::binary | std::ios::trunc) << text;
        }

        /// Initializes the singleton runner against this directory with a fixed capture.
        Spark::GoldenImageTestRunner& Runner(const std::vector<uint8_t>& capture, const std::string& row = kRow) const
        {
            Spark::GoldenImageConfig config;
            config.goldenImageDir = root.string();
            config.outputDir = (root / "output").string();
            config.backendRow = row;
            auto& runner = Spark::GoldenImageTestRunner::GetInstance();
            runner.Initialize(config);
            runner.SetCapture(std::make_unique<FixedCapture>(capture));
            return runner;
        }
    };

    std::string EntryJson(const std::string& scene, const std::string& sha, double perPixel, double tolerance)
    {
        return "{\"scene\": \"" + scene + "\", \"backendRow\": \"" + kRow +
               "\", \"software\": true, \"perPixelThreshold\": " + std::to_string(perPixel) +
               ", \"tolerancePercent\": " + std::to_string(tolerance) +
               ", \"reviewer\": \"golden-test\", \"baselineSha256\": \"" + sha + "\"}";
    }

    std::string ManifestJson(const std::string& entries)
    {
        return "{\"schemaVersion\": 1, \"entries\": [" + entries + "]}";
    }

    /// True when a manifest with exactly this text is rejected.
    bool ManifestRejected(const GoldenScratch& scratch, const std::string& text)
    {
        scratch.WriteManifest(text);
        std::vector<Spark::GoldenManifestEntry> entries;
        std::string error;
        const bool loaded = Spark::GoldenManifest::Load(scratch.root / "manifest.json", entries, error);
        return !loaded && entries.empty() && !error.empty();
    }

    const std::string kZeroSha(64, '0');
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
    EXPECT_EQ(config.maxDiffsToReport, 100u);
    EXPECT_TRUE(config.goldenImageDir.empty());
    EXPECT_TRUE(config.outputDir.empty());
    EXPECT_TRUE(config.backendRow.empty());
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

// ============================================================================
// Real PNG encode/decode
// ============================================================================

TEST(GoldenImageTest_PNG_RoundTripWritesRealPng)
{
    GoldenScratch scratch("png-roundtrip");
    const auto pixels = MakeGradient();
    const auto path = scratch.root / "roundtrip.png";
    ASSERT_TRUE(Spark::GoldenImageTestRunner::SavePNG(path.string(), pixels.data(), kSceneSize, kSceneSize));

    // A real PNG: 8-byte signature followed by the IHDR chunk carrying the size.
    std::ifstream in(path, std::ios::binary);
    std::vector<unsigned char> header(24);
    in.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
    ASSERT_EQ(static_cast<size_t>(in.gcount()), header.size());
    const std::vector<unsigned char> signature = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    EXPECT_TRUE(std::equal(signature.begin(), signature.end(), header.begin()));
    EXPECT_EQ(std::string(header.begin() + 12, header.begin() + 16), std::string("IHDR"));
    EXPECT_EQ(header[19], static_cast<unsigned char>(kSceneSize)); // width, big-endian
    EXPECT_EQ(header[23], static_cast<unsigned char>(kSceneSize)); // height, big-endian

    uint32_t w = 0;
    uint32_t h = 0;
    const auto decoded = Spark::GoldenImageTestRunner::LoadPNG(path.string(), w, h);
    EXPECT_EQ(w, kSceneSize);
    EXPECT_EQ(h, kSceneSize);
    EXPECT_TRUE(decoded == pixels);
}

TEST(GoldenImageTest_PNG_LegacyRawRgbaIsRejected)
{
    GoldenScratch scratch("png-legacy");
    // The pre-RHI-210 on-disk layout: [width:4][height:4][RGBA...] under a .png name.
    const auto path = scratch.root / "legacy.png";
    {
        std::ofstream out(path, std::ios::binary);
        const uint32_t size = 2;
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        out.write(reinterpret_cast<const char*>(&size), sizeof(size));
        const std::vector<char> rgba(2 * 2 * 4, 0x7F);
        out.write(rgba.data(), static_cast<std::streamsize>(rgba.size()));
    }
    uint32_t w = 7;
    uint32_t h = 7;
    EXPECT_TRUE(Spark::GoldenImageTestRunner::LoadPNG(path.string(), w, h).empty());
    EXPECT_EQ(w, 0u);
    EXPECT_EQ(h, 0u);
}

namespace
{
    void AppendBE32(std::vector<uint8_t>& out, uint32_t value)
    {
        out.push_back(static_cast<uint8_t>(value >> 24));
        out.push_back(static_cast<uint8_t>(value >> 16));
        out.push_back(static_cast<uint8_t>(value >> 8));
        out.push_back(static_cast<uint8_t>(value));
    }

    void AppendChunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data)
    {
        AppendBE32(out, static_cast<uint32_t>(data.size()));
        std::vector<uint8_t> typed(type, type + 4);
        typed.insert(typed.end(), data.begin(), data.end());
        out.insert(out.end(), typed.begin(), typed.end());
        AppendBE32(out, static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, typed.data(), typed.size())));
    }

    /// Builds an 8-bit RGB PNG whose five rows use filter types 0..4, from already-filtered scanlines.
    std::vector<uint8_t> BuildFilteredRgbPng(const std::vector<uint8_t>& filteredRows, uint32_t width, uint32_t height)
    {
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<uint8_t> ihdr;
        AppendBE32(ihdr, width);
        AppendBE32(ihdr, height);
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
        AppendChunk(png, "IHDR", ihdr);

        mz_ulong compressedSize = mz_compressBound(static_cast<mz_ulong>(filteredRows.size()));
        std::vector<uint8_t> compressed(compressedSize);
        if (mz_compress(compressed.data(), &compressedSize, filteredRows.data(),
                        static_cast<mz_ulong>(filteredRows.size())) != MZ_OK)
            return {};
        compressed.resize(compressedSize);
        AppendChunk(png, "IDAT", compressed);
        AppendChunk(png, "IEND", {});
        return png;
    }

    bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        return out.good();
    }
} // namespace

TEST(GoldenImageTest_PNG_DecodesAllScanlineFilters)
{
    // 2x5 RGB image; every pixel channel is distinct so a wrong predictor shows.
    constexpr uint32_t width = 2;
    constexpr uint32_t height = 5;
    constexpr size_t stride = width * 3;
    std::vector<uint8_t> expected(stride * height);
    for (size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<uint8_t>(17 + i * 23);

    auto paeth = [](int a, int b, int c)
    {
        const int p = a + b - c;
        const int pa = std::abs(p - a);
        const int pb = std::abs(p - b);
        const int pc = std::abs(p - c);
        return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
    };

    // Row y uses filter type y (None, Sub, Up, Average, Paeth).
    std::vector<uint8_t> filtered;
    for (uint32_t y = 0; y < height; ++y)
    {
        filtered.push_back(static_cast<uint8_t>(y));
        for (size_t x = 0; x < stride; ++x)
        {
            const int value = expected[y * stride + x];
            const int left = x >= 3 ? expected[y * stride + x - 3] : 0;
            const int up = y > 0 ? expected[(y - 1) * stride + x] : 0;
            const int upLeft = (y > 0 && x >= 3) ? expected[(y - 1) * stride + x - 3] : 0;
            const int predictor = y == 0   ? 0
                                  : y == 1 ? left
                                  : y == 2 ? up
                                  : y == 3 ? (left + up) / 2
                                           : paeth(left, up, upLeft);
            filtered.push_back(static_cast<uint8_t>(value - predictor));
        }
    }

    GoldenScratch scratch("png-filters");
    const auto png = BuildFilteredRgbPng(filtered, width, height);
    ASSERT_FALSE(png.empty());
    const auto path = scratch.root / "filters.png";
    ASSERT_TRUE(WriteBytes(path, png));

    uint32_t w = 0;
    uint32_t h = 0;
    const auto rgba = Spark::GoldenImageTestRunner::LoadPNG(path.string(), w, h);
    ASSERT_EQ(rgba.size(), size_t(width) * height * 4);
    EXPECT_EQ(w, width);
    EXPECT_EQ(h, height);
    bool allMatch = true;
    for (size_t i = 0; i < size_t(width) * height; ++i)
    {
        allMatch = allMatch && rgba[i * 4 + 0] == expected[i * 3 + 0] && rgba[i * 4 + 1] == expected[i * 3 + 1] &&
                   rgba[i * 4 + 2] == expected[i * 3 + 2] && rgba[i * 4 + 3] == 255;
    }
    EXPECT_TRUE(allMatch);

    // A corrupted chunk CRC, a truncated file, and an unknown filter type are all rejected.
    auto badCrc = png;
    badCrc[8 + 8 + 13] ^= 0xFF; // first byte of the IHDR CRC
    ASSERT_TRUE(WriteBytes(path, badCrc));
    EXPECT_TRUE(Spark::GoldenImageTestRunner::LoadPNG(path.string(), w, h).empty());

    const std::vector<uint8_t> truncated(png.begin(), png.end() - 6);
    ASSERT_TRUE(WriteBytes(path, truncated));
    EXPECT_TRUE(Spark::GoldenImageTestRunner::LoadPNG(path.string(), w, h).empty());

    auto badFilter = filtered;
    badFilter[0] = 5;
    ASSERT_TRUE(WriteBytes(path, BuildFilteredRgbPng(badFilter, width, height)));
    EXPECT_TRUE(Spark::GoldenImageTestRunner::LoadPNG(path.string(), w, h).empty());
    EXPECT_EQ(w, 0u);
}

// ============================================================================
// Reviewed-threshold manifest
// ============================================================================

TEST(GoldenImageTest_Manifest_RepositoryManifestIsValid)
{
    const std::filesystem::path manifest =
        std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Tests" / "GoldenImages" / "manifest.json";
    std::vector<Spark::GoldenManifestEntry> entries;
    std::string error;
    EXPECT_TRUE(Spark::GoldenManifest::Load(manifest, entries, error));
    EXPECT_TRUE(error.empty());
}

TEST(GoldenImageTest_Manifest_ValidEntryParses)
{
    GoldenScratch scratch("manifest-valid");
    scratch.WriteManifest(ManifestJson(EntryJson("Scene_A-1", kZeroSha, 12.5, 0.25)));
    std::vector<Spark::GoldenManifestEntry> entries;
    std::string error;
    ASSERT_TRUE(Spark::GoldenManifest::Load(scratch.root / "manifest.json", entries, error));
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].scene, std::string("Scene_A-1"));
    EXPECT_EQ(entries[0].backendRow, std::string(kRow));
    EXPECT_TRUE(entries[0].software);
    EXPECT_NEAR(entries[0].perPixelThreshold, 12.5f, 0.0001f);
    EXPECT_NEAR(entries[0].tolerancePercent, 0.25f, 0.0001f);
    EXPECT_EQ(entries[0].reviewer, std::string("golden-test"));
    EXPECT_EQ(entries[0].baselineSha256, kZeroSha);
}

TEST(GoldenImageTest_Manifest_InvalidManifestsFailClosed)
{
    GoldenScratch scratch("manifest-invalid");
    const std::string good = EntryJson("scene", kZeroSha, 10, 0.5);

    EXPECT_TRUE(ManifestRejected(scratch, "not json"));
    EXPECT_TRUE(ManifestRejected(scratch, "{\"schemaVersion\": 2, \"entries\": []}"));
    EXPECT_TRUE(ManifestRejected(scratch, "{\"entries\": []}"));
    EXPECT_TRUE(ManifestRejected(scratch, "{\"schemaVersion\": 1, \"entries\": [], \"extra\": 1}"));
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(good + ", " + good)));                        // duplicate
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(EntryJson("../escape", kZeroSha, 10, 0.5)))); // scene id
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(EntryJson("scene", "ABC", 10, 0.5))));        // hash
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(EntryJson("scene", kZeroSha, -1, 0.5))));     // perPixel
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(EntryJson("scene", kZeroSha, 500, 0.5))));    // perPixel
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(EntryJson("scene", kZeroSha, 10, 101))));     // tolerance

    std::string misspelled = good;
    misspelled.replace(misspelled.find("tolerancePercent"), 16, "tolerancePct");
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(misspelled)));

    std::string wrongRow = good;
    wrongRow.replace(wrongRow.find(kRow), std::string(kRow).size(), "metal-hw");
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(wrongRow)));

    std::string hardwareClaimedSoftware = good;
    hardwareClaimedSoftware.replace(hardwareClaimedSoftware.find(kRow), std::string(kRow).size(), "d3d11-hw");
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(hardwareClaimedSoftware)));

    std::string noReviewer = good;
    noReviewer.replace(noReviewer.find("golden-test"), 11, " ");
    EXPECT_TRUE(ManifestRejected(scratch, ManifestJson(noReviewer)));
}

// ============================================================================
// CompareWithGolden — fail-closed gate and manifest thresholds
// ============================================================================

TEST(GoldenImageTest_CompareWithGolden_MissingManifestFails)
{
    GoldenScratch scratch("compare-no-manifest");
    const auto pixels = MakeGradient();
    ASSERT_FALSE(scratch.WriteBaseline("scene", pixels).empty());

    const auto result = scratch.Runner(pixels).CompareWithGolden("scene");
    EXPECT_FALSE(result.matched);
    EXPECT_TRUE(result.failureReason.find("manifest") != std::string::npos);
}

TEST(GoldenImageTest_CompareWithGolden_MissingEntryFails)
{
    GoldenScratch scratch("compare-no-entry");
    const auto pixels = MakeGradient();
    const std::string sha = scratch.WriteBaseline("scene", pixels);
    ASSERT_FALSE(sha.empty());
    scratch.WriteManifest(ManifestJson(EntryJson("other", sha, 10, 0.5)));

    const auto result = scratch.Runner(pixels).CompareWithGolden("scene");
    EXPECT_FALSE(result.matched);
    EXPECT_TRUE(result.failureReason.find("no manifest entry") != std::string::npos);

    // The same entry does not cover another backend row.
    scratch.WriteManifest(ManifestJson(EntryJson("scene", sha, 10, 0.5)));
    const auto otherRow = scratch.Runner(pixels, "opengl-llvmpipe").CompareWithGolden("scene");
    EXPECT_FALSE(otherRow.matched);

    const auto unknownRow = scratch.Runner(pixels, "").CompareWithGolden("scene");
    EXPECT_FALSE(unknownRow.matched);
    EXPECT_TRUE(unknownRow.failureReason.find("backend row") != std::string::npos);
}

TEST(GoldenImageTest_CompareWithGolden_MissingBaselineFails)
{
    GoldenScratch scratch("compare-no-baseline");
    scratch.WriteManifest(ManifestJson(EntryJson("scene", kZeroSha, 10, 0.5)));

    const auto result = scratch.Runner(MakeGradient()).CompareWithGolden("scene");
    EXPECT_FALSE(result.matched);
    EXPECT_TRUE(result.failureReason.find("baseline unreadable") != std::string::npos);
}

TEST(GoldenImageTest_CompareWithGolden_WrongBaselineHashFails)
{
    GoldenScratch scratch("compare-wrong-hash");
    const auto pixels = MakeGradient();
    const std::string sha = scratch.WriteBaseline("scene", pixels);
    ASSERT_FALSE(sha.empty());

    // An identical capture still fails when the committed baseline is not the reviewed one.
    std::string wrong = sha;
    wrong[0] = (wrong[0] == '0') ? '1' : '0';
    scratch.WriteManifest(ManifestJson(EntryJson("scene", wrong, 10, 0.5)));
    const auto result = scratch.Runner(pixels).CompareWithGolden("scene");
    EXPECT_FALSE(result.matched);
    EXPECT_TRUE(result.failureReason.find("SHA-256") != std::string::npos);

    scratch.WriteManifest(ManifestJson(EntryJson("scene", sha, 10, 0.5)));
    const auto reviewed = scratch.Runner(pixels).CompareWithGolden("scene");
    EXPECT_TRUE(reviewed.matched);
    EXPECT_TRUE(reviewed.failureReason.empty());
    EXPECT_EQ(reviewed.differentPixels, 0u);
}

TEST(GoldenImageTest_CompareWithGolden_ThresholdsComeFromManifest)
{
    GoldenScratch scratch("compare-thresholds");
    const auto baseline = MakeGradient();
    const std::string sha = scratch.WriteBaseline("strict", baseline);
    ASSERT_FALSE(sha.empty());
    ASSERT_EQ(scratch.WriteBaseline("perpixel", baseline), sha);
    ASSERT_EQ(scratch.WriteBaseline("tolerant", baseline), sha);

    // One of 100 pixels moves by distance 20 in red: 1% of pixels differ.
    auto actual = baseline;
    actual[0] = static_cast<uint8_t>(actual[0] + 20);

    scratch.WriteManifest(ManifestJson(EntryJson("strict", sha, 5, 0.5) + ", " + EntryJson("perpixel", sha, 25, 0.0) +
                                       ", " + EntryJson("tolerant", sha, 5, 1.0)));
    auto& runner = scratch.Runner(actual);

    const auto strict = runner.CompareWithGolden("strict");
    EXPECT_FALSE(strict.matched);
    EXPECT_NEAR(strict.perPixelThreshold, 5.0f, 0.0001f);
    EXPECT_NEAR(strict.tolerancePercent, 0.5f, 0.0001f);
    EXPECT_EQ(strict.differentPixels, 1u);
    EXPECT_NEAR(strict.percentDifferent, 1.0f, 0.001f);
    EXPECT_TRUE(std::filesystem::exists(strict.diffImagePath));

    const auto perPixel = runner.CompareWithGolden("perpixel");
    EXPECT_TRUE(perPixel.matched);
    EXPECT_NEAR(perPixel.perPixelThreshold, 25.0f, 0.0001f);
    EXPECT_EQ(perPixel.differentPixels, 0u);

    const auto tolerant = runner.CompareWithGolden("tolerant");
    EXPECT_TRUE(tolerant.matched);
    EXPECT_NEAR(tolerant.tolerancePercent, 1.0f, 0.0001f);
    EXPECT_EQ(tolerant.differentPixels, 1u);

    const auto all = runner.RunAllComparisons();
    EXPECT_EQ(all.size(), 3u);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::HasRegressions(all));
}

// ============================================================================
// Frame content (shared blank-frame rejection used by backend golden tests)
// ============================================================================

TEST(GoldenImageTest_FrameContent_UniformFrameRejected)
{
    std::vector<uint8_t> frame(8 * 8 * 4, 40);
    EXPECT_FALSE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.95));
    EXPECT_FALSE(Spark::GoldenImageTestRunner::FrameHasRenderedContent({}, 0.95));

    // Paint 16 of 64 pixels a second colour: 75% dominant, 2 colours.
    for (size_t i = 0; i < 16; ++i)
        frame[i * 4] = 200;
    const auto content = Spark::GoldenImageTestRunner::AnalyzeFrame(frame);
    EXPECT_EQ(content.distinctColors, 2u);
    EXPECT_NEAR(content.dominantFraction, 0.75, 0.0001);
    EXPECT_TRUE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.95));
    EXPECT_FALSE(Spark::GoldenImageTestRunner::FrameHasRenderedContent(frame, 0.5));
}
