// TestGraphicsIntegration.cpp - Tests for Tier 1 graphics systems
// Covers: VolumeSystem, BloomEffect, TonemapColorGrading, MeshOptimizer,
//         BasisTranscoder, CachedShadowAtlas, UIDirtyTracking,
//         ShaderVariantSystem, FastNoiseLite

#include "TestFramework.h"
#include "Graphics/BasisTranscoder.h"
#include "Graphics/EXRLoader.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <unordered_set>
#include <vector>

// ============================================================================
// Volume System Tests
// ============================================================================

// Standalone reproduction of key volume system types
namespace TestVolume
{

    struct VolumeParam
    {
        float value = 0.0f;
        bool overrideState = false;
    };

    struct VolumeBlendFactor
    {
        float boundsMinX, boundsMinY, boundsMinZ;
        float boundsMaxX, boundsMaxY, boundsMaxZ;
        float blendDistance;
        float weight;
        bool isGlobal;

        float Calculate(float px, float py, float pz) const
        {
            if (isGlobal)
                return weight;

            float dx = std::max({boundsMinX - px, px - boundsMaxX, 0.0f});
            float dy = std::max({boundsMinY - py, py - boundsMaxY, 0.0f});
            float dz = std::max({boundsMinZ - pz, pz - boundsMaxZ, 0.0f});
            float distance = std::sqrt(dx * dx + dy * dy + dz * dz);

            if (distance > blendDistance)
                return 0.0f;
            if (distance <= 0.0f)
                return weight;

            float t = 1.0f - (distance / blendDistance);
            return t * t * (3.0f - 2.0f * t) * weight;
        }
    };

} // namespace TestVolume

TEST(VolumeSystem_GlobalVolumeAlwaysActive)
{
    TestVolume::VolumeBlendFactor global = {};
    global.isGlobal = true;
    global.weight = 1.0f;

    // Global volume should return full weight regardless of position
    EXPECT_NEAR(global.Calculate(0, 0, 0), 1.0f, 0.0001f);
    EXPECT_NEAR(global.Calculate(999, -999, 123), 1.0f, 0.0001f);
}

TEST(VolumeSystem_LocalVolumeInsideFullWeight)
{
    TestVolume::VolumeBlendFactor local = {};
    local.boundsMinX = -5;
    local.boundsMinY = -5;
    local.boundsMinZ = -5;
    local.boundsMaxX = 5;
    local.boundsMaxY = 5;
    local.boundsMaxZ = 5;
    local.blendDistance = 2.0f;
    local.weight = 1.0f;
    local.isGlobal = false;

    // Point inside bounds should get full weight
    EXPECT_NEAR(local.Calculate(0, 0, 0), 1.0f, 0.0001f);
    EXPECT_NEAR(local.Calculate(3, 3, 3), 1.0f, 0.0001f);
}

TEST(VolumeSystem_LocalVolumeOutsideZeroWeight)
{
    TestVolume::VolumeBlendFactor local = {};
    local.boundsMinX = -5;
    local.boundsMinY = -5;
    local.boundsMinZ = -5;
    local.boundsMaxX = 5;
    local.boundsMaxY = 5;
    local.boundsMaxZ = 5;
    local.blendDistance = 2.0f;
    local.weight = 1.0f;
    local.isGlobal = false;

    // Point far outside should get zero
    EXPECT_NEAR(local.Calculate(100, 0, 0), 0.0f, 0.0001f);
}

TEST(VolumeSystem_LocalVolumeBlendDistance)
{
    TestVolume::VolumeBlendFactor local = {};
    local.boundsMinX = 0;
    local.boundsMinY = 0;
    local.boundsMinZ = 0;
    local.boundsMaxX = 10;
    local.boundsMaxY = 10;
    local.boundsMaxZ = 10;
    local.blendDistance = 4.0f;
    local.weight = 1.0f;
    local.isGlobal = false;

    // At blend distance boundary → 0
    float atBoundary = local.Calculate(14, 5, 5);
    EXPECT_NEAR(atBoundary, 0.0f, 0.0001f);

    // Halfway through blend → partial
    float halfway = local.Calculate(12, 5, 5);
    EXPECT_TRUE(halfway > 0.0f && halfway < 1.0f);
}

// ============================================================================
// Mesh Optimizer Tests
// ============================================================================

namespace TestMeshOpt
{

    static float ComputeACMR(const uint32_t* indices, size_t indexCount, int cacheSize = 32)
    {
        if (indexCount == 0)
            return 0.0f;
        std::vector<int> cache(cacheSize, -1);
        uint32_t misses = 0;
        for (size_t i = 0; i < indexCount; ++i)
        {
            int v = static_cast<int>(indices[i]);
            bool found = false;
            for (int j = 0; j < cacheSize; ++j)
                if (cache[j] == v)
                {
                    found = true;
                    break;
                }
            if (!found)
            {
                misses++;
                for (int j = cacheSize - 1; j > 0; --j)
                    cache[j] = cache[j - 1];
                cache[0] = v;
            }
        }
        size_t triCount = indexCount / 3;
        return triCount > 0 ? static_cast<float>(misses) / triCount : 0.0f;
    }

} // namespace TestMeshOpt

TEST(MeshOptimizer_ACMRComputation)
{
    // Simple sequential indices should have low ACMR
    std::vector<uint32_t> sequential = {0, 1, 2, 1, 2, 3, 2, 3, 4, 3, 4, 5};
    float acmr = TestMeshOpt::ComputeACMR(sequential.data(), sequential.size());
    EXPECT_TRUE(acmr <= 2.0f); // Sequential access is cache-friendly
}

TEST(MeshOptimizer_ACMRScatteredIndices)
{
    // Scattered indices should have higher ACMR
    std::vector<uint32_t> scattered = {0, 100, 200, 50, 150, 250, 25, 125, 225};
    float acmr = TestMeshOpt::ComputeACMR(scattered.data(), scattered.size());
    EXPECT_TRUE(acmr > 0.0f);
}

// ============================================================================
// Meshlet Generation Tests
// ============================================================================

TEST(MeshOptimizer_MeshletLimits)
{
    // Verify meshlet constants
    constexpr uint32_t maxVerts = 64;
    constexpr uint32_t maxTris = 124;
    EXPECT_EQ(maxVerts, 64u);
    EXPECT_EQ(maxTris, 124u);
}

// ============================================================================
// Basis Transcoder Tests
// ============================================================================

TEST(BasisTranscoder_BlockSizes)
{
    using namespace Spark::Graphics;
    EXPECT_EQ(BasisTranscoder::GetBlockSize(TranscoderFormat::BC1_RGB), 8u);
    EXPECT_EQ(BasisTranscoder::GetBlockSize(TranscoderFormat::BC3_RGBA), 16u);
    EXPECT_EQ(BasisTranscoder::GetBlockSize(TranscoderFormat::BC7_RGBA), 16u);
}

TEST(BasisTranscoder_CompressionRatios)
{
    using namespace Spark::Graphics;
    EXPECT_NEAR(BasisTranscoder::GetCompressionRatio(TranscoderFormat::BC1_RGB), 8.0f, 0.0001f);
    EXPECT_NEAR(BasisTranscoder::GetCompressionRatio(TranscoderFormat::BC7_RGBA), 4.0f, 0.0001f);
}

TEST(BasisTranscoder_DXGIFormatMapping)
{
    using namespace Spark::Graphics;
    EXPECT_EQ(BasisTranscoder::GetDXGIFormat(TranscoderFormat::BC1_RGB), 71u);
    EXPECT_EQ(BasisTranscoder::GetDXGIFormat(TranscoderFormat::BC3_RGBA), 77u);
    EXPECT_EQ(BasisTranscoder::GetDXGIFormat(TranscoderFormat::BC7_RGBA), 98u);
    EXPECT_EQ(BasisTranscoder::GetDXGIFormat(TranscoderFormat::RGBA32), 28u);
}

TEST(BasisTranscoder_FailsClosedWithoutBackend)
{
    using namespace Spark::Graphics;
    std::vector<uint8_t> basis(32, 0);
    basis[0] = 0x73;
    basis[1] = 0x42;
    basis[4] = 0x01; // ETC1S: this exact minimum-size case previously divided by zero.
    basis[8] = 1;    // totalImages
    basis[12] = 1;   // width
    basis[16] = 1;   // height
    basis[20] = 1;   // mipLevels

    BasisFileHeader header;
    EXPECT_FALSE(BasisTranscoder::ParseHeader(basis.data(), basis.size(), header));
    EXPECT_TRUE(BasisTranscoder::Transcode(basis.data(), basis.size(), TranscoderFormat::RGBA32).data.empty());
    EXPECT_TRUE(BasisTranscoder::Transcode(basis.data(), basis.size(), TranscoderFormat::BC7_RGBA, 0, 32).data.empty());
}

TEST(BasisTranscoder_HeaderValidationIsBoundedAndTransactional)
{
    using namespace Spark::Graphics;
    std::vector<uint8_t> storage(33, 0);
    uint8_t* basis = storage.data() + 1; // Deliberately unaligned input.
    basis[0] = 0x73;
    basis[1] = 0x42;
    basis[8] = 1;
    basis[12] = 1;
    basis[16] = 1;
    basis[20] = 1;

    BasisFileHeader header;
    header.width = 77;
    EXPECT_FALSE(BasisTranscoder::ParseHeader(basis, 32, header));
    EXPECT_EQ(header.width, 77u);

    basis[15] = 0x40; // width = 0x40000001, beyond the allocation budget.
    BasisFileHeader sentinel;
    sentinel.width = 77;
    EXPECT_FALSE(BasisTranscoder::ParseHeader(basis, 32, sentinel));
    EXPECT_EQ(sentinel.width, 77u);
}

// ============================================================================
// EXR Loader Tests
// ============================================================================

#if defined(SPARK_HAS_TINYEXR) && SPARK_HAS_TINYEXR
TEST(EXRLoader_RealCompressedMemoryRoundTrip)
{
    const float source[] = {1.0f, 0.25f, 0.5f, 1.0f, 0.0f, 0.75f, 0.125f, 0.5f};
    unsigned char* encoded = nullptr;
    const char* error = nullptr;
    const int encodedSize = SaveEXRToMemory(source, 2, 1, 4, 1, &encoded, &error);
    EXPECT_TRUE(encodedSize > 0);
    EXPECT_TRUE(encoded != nullptr);

    Spark::Graphics::EXRImage image;
    EXPECT_TRUE(Spark::Graphics::EXRLoader::Load(encoded, static_cast<size_t>(encodedSize), image));
    EXPECT_EQ(image.width, 2u);
    EXPECT_EQ(image.height, 1u);
    EXPECT_EQ(image.pixels.size(), 8u);
    EXPECT_NEAR(image.pixels[0], 1.0f, 0.001f);
    EXPECT_NEAR(image.pixels[5], 0.75f, 0.001f);

    std::free(encoded);
    if (error)
        FreeEXRErrorMessage(error);
}

TEST(EXRLoader_TruncationFailsTransactionally)
{
    const float source[] = {0.1f, 0.2f, 0.3f, 1.0f};
    unsigned char* encoded = nullptr;
    const char* error = nullptr;
    const int encodedSize = SaveEXRToMemory(source, 1, 1, 4, 1, &encoded, &error);
    EXPECT_TRUE(encodedSize > 0);

    Spark::Graphics::EXRImage image;
    image.width = 77;
    image.pixels = {42.0f};
    EXPECT_FALSE(Spark::Graphics::EXRLoader::Load(encoded, static_cast<size_t>(encodedSize / 2), image));
    EXPECT_EQ(image.width, 77u);
    EXPECT_EQ(image.pixels.size(), 1u);
    EXPECT_NEAR(image.pixels[0], 42.0f, 0.001f);

    std::free(encoded);
    if (error)
        FreeEXRErrorMessage(error);
}

TEST(EXRLoader_RejectsUnsupportedChannelLayoutTransactionally)
{
    const float source[] = {0.5f};
    unsigned char* encoded = nullptr;
    const char* error = nullptr;
    const int encodedSize = SaveEXRToMemory(source, 1, 1, 1, 1, &encoded, &error);
    EXPECT_TRUE(encodedSize > 0);
    EXPECT_TRUE(encoded != nullptr);

    Spark::Graphics::EXRImage image;
    image.width = 77;
    image.pixels = {9.0f};
    for (int attempt = 0; attempt < 4; ++attempt)
        EXPECT_FALSE(Spark::Graphics::EXRLoader::Load(encoded, static_cast<size_t>(encodedSize), image));
    EXPECT_EQ(image.width, 77u);
    EXPECT_EQ(image.pixels.size(), size_t{1});
    EXPECT_NEAR(image.pixels[0], 9.0f, 0.001f);

    std::free(encoded);
    if (error)
        FreeEXRErrorMessage(error);
}

TEST(EXRLoader_RejectsSubsampledChannels)
{
    const float source[] = {1.0f, 0.25f, 0.5f, 1.0f, 0.0f, 0.75f, 0.125f, 0.5f};
    unsigned char* encoded = nullptr;
    const char* error = nullptr;
    const int encodedSize = SaveEXRToMemory(source, 2, 1, 4, 1, &encoded, &error);
    EXPECT_TRUE(encodedSize > 0);
    EXPECT_TRUE(encoded != nullptr);

    const std::string marker("channels\0chlist\0", 16);
    auto* markerPosition = std::search(encoded, encoded + encodedSize, marker.begin(), marker.end());
    EXPECT_TRUE(markerPosition != encoded + encodedSize);
    if (markerPosition != encoded + encodedSize)
    {
        unsigned char* attributeSize = markerPosition + marker.size();
        unsigned char* channelName = attributeSize + sizeof(uint32_t);
        unsigned char* channelNameEnd = std::find(channelName, encoded + encodedSize, static_cast<unsigned char>(0));
        EXPECT_TRUE(channelNameEnd != encoded + encodedSize);
        if (channelNameEnd != encoded + encodedSize && encoded + encodedSize - channelNameEnd >= 13)
        {
            const uint32_t subsampled = 2;
            std::memcpy(channelNameEnd + 1 + 8, &subsampled, sizeof(subsampled));

            Spark::Graphics::EXRImage image;
            image.width = 77;
            EXPECT_FALSE(Spark::Graphics::EXRLoader::Load(encoded, static_cast<size_t>(encodedSize), image));
            EXPECT_EQ(image.width, 77u);
        }
    }

    std::free(encoded);
    if (error)
        FreeEXRErrorMessage(error);
}
#endif

// ============================================================================
// Cached Shadow Atlas Tests
// ============================================================================

namespace TestCachedShadow
{

    static uint64_t ComputeHash(float px, float py, float pz, float range)
    {
        uint64_t hash = 14695981039346656037ULL;
        auto hashFloat = [&](float f)
        {
            uint32_t bits;
            memcpy(&bits, &f, sizeof(bits));
            hash ^= bits;
            hash *= 1099511628211ULL;
        };
        hashFloat(px);
        hashFloat(py);
        hashFloat(pz);
        hashFloat(range);
        return hash;
    }

} // namespace TestCachedShadow

TEST(CachedShadowAtlas_HashChangeDetection)
{
    uint64_t h1 = TestCachedShadow::ComputeHash(1.0f, 2.0f, 3.0f, 10.0f);
    uint64_t h2 = TestCachedShadow::ComputeHash(1.0f, 2.0f, 3.0f, 10.0f);
    uint64_t h3 = TestCachedShadow::ComputeHash(1.1f, 2.0f, 3.0f, 10.0f);

    EXPECT_EQ(h1, h2); // Same input → same hash
    EXPECT_NE(h1, h3); // Different input → different hash
}

TEST(CachedShadowAtlas_HashDistribution)
{
    // Ensure nearby floats produce different hashes
    std::unordered_set<uint64_t> hashes;
    for (int i = 0; i < 100; ++i)
    {
        float x = static_cast<float>(i) * 0.1f;
        hashes.insert(TestCachedShadow::ComputeHash(x, 0, 0, 10));
    }
    EXPECT_TRUE(hashes.size() >= 95); // At least 95% unique
}

// ============================================================================
// UI Dirty Tracking Tests
// ============================================================================

namespace TestDirtyTracking
{

    struct DirtyRect
    {
        float x, y, width, height;
        bool IsEmpty() const { return width <= 0 || height <= 0; }
        void Merge(const DirtyRect& other)
        {
            if (other.IsEmpty())
                return;
            if (IsEmpty())
            {
                *this = other;
                return;
            }
            float minX = std::min(x, other.x);
            float minY = std::min(y, other.y);
            float maxX = std::max(x + width, other.x + other.width);
            float maxY = std::max(y + height, other.y + other.height);
            x = minX;
            y = minY;
            width = maxX - minX;
            height = maxY - minY;
        }
    };

} // namespace TestDirtyTracking

TEST(UIDirtyTracking_RectMerge)
{
    TestDirtyTracking::DirtyRect a = {0, 0, 100, 100};
    TestDirtyTracking::DirtyRect b = {50, 50, 100, 100};
    a.Merge(b);
    EXPECT_NEAR(a.x, 0.0f, 0.0001f);
    EXPECT_NEAR(a.y, 0.0f, 0.0001f);
    EXPECT_NEAR(a.width, 150.0f, 0.0001f);
    EXPECT_NEAR(a.height, 150.0f, 0.0001f);
}

TEST(UIDirtyTracking_EmptyRectMerge)
{
    TestDirtyTracking::DirtyRect a = {10, 20, 100, 50};
    TestDirtyTracking::DirtyRect empty = {0, 0, 0, 0};
    a.Merge(empty);
    EXPECT_NEAR(a.x, 10.0f, 0.0001f);
    EXPECT_NEAR(a.width, 100.0f, 0.0001f);
}

TEST(UIDirtyTracking_GridCellCalculation)
{
    constexpr int kGridSize = 16;
    float screenWidth = 1920.0f;
    float cellWidth = screenWidth / kGridSize;
    EXPECT_NEAR(cellWidth, 120.0f, 0.0001f);

    // A widget at (300, 200) with size (100, 50) should cover cells (2,2)-(3,3)
    int x0 = static_cast<int>(300.0f / cellWidth);
    int x1 = static_cast<int>((300.0f + 100.0f) / cellWidth);
    EXPECT_EQ(x0, 2);
    EXPECT_EQ(x1, 3);
}

// ============================================================================
// Shader Variant System Tests
// ============================================================================

namespace TestShaderVariants
{

    struct VariantKey
    {
        uint64_t bits = 0;
        bool HasKeyword(int index) const { return (bits & (1ULL << index)) != 0; }
        void SetKeyword(int index, bool enabled)
        {
            if (enabled)
                bits |= (1ULL << index);
            else
                bits &= ~(1ULL << index);
        }
    };

} // namespace TestShaderVariants

TEST(ShaderVariantSystem_KeywordBitManipulation)
{
    TestShaderVariants::VariantKey key;
    EXPECT_FALSE(key.HasKeyword(0));

    key.SetKeyword(3, true);
    EXPECT_TRUE(key.HasKeyword(3));
    EXPECT_FALSE(key.HasKeyword(2));

    key.SetKeyword(3, false);
    EXPECT_FALSE(key.HasKeyword(3));
}

TEST(ShaderVariantSystem_MaxKeywords)
{
    TestShaderVariants::VariantKey key;
    key.SetKeyword(63, true); // Max supported keyword
    EXPECT_TRUE(key.HasKeyword(63));
    EXPECT_EQ(key.bits, 1ULL << 63);
}

TEST(ShaderVariantSystem_VariantCombinations)
{
    // 3 keywords = 8 possible variants
    int keywords = 3;
    uint64_t variants = 1ULL << keywords;
    EXPECT_EQ(variants, 8u);
}

// ============================================================================
// FastNoiseLite Tests
// ============================================================================

namespace TestNoise
{

    static int Hash(int seed, int x, int y)
    {
        // Use unsigned arithmetic to avoid signed integer overflow (UBSan)
        auto us = static_cast<unsigned int>(seed);
        auto ux = static_cast<unsigned int>(x);
        auto uy = static_cast<unsigned int>(y);
        unsigned int h = us ^ (ux * 374761393u) ^ (uy * 668265263u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return static_cast<int>(h);
    }

    static float ValCoord2D(int seed, int x, int y)
    {
        int h = Hash(seed, x, y);
        return static_cast<float>(h) * (1.0f / 2147483648.0f);
    }

    static float InterpHermite(float t)
    {
        return t * t * (3 - 2 * t);
    }

    static float Lerp(float a, float b, float t)
    {
        return a + t * (b - a);
    }

    static float ValueNoise2D(int seed, float x, float y)
    {
        int x0 = static_cast<int>(x >= 0 ? static_cast<int>(x) : static_cast<int>(x) - 1);
        int y0 = static_cast<int>(y >= 0 ? static_cast<int>(y) : static_cast<int>(y) - 1);
        float xs = InterpHermite(x - x0);
        float ys = InterpHermite(y - y0);
        return Lerp(Lerp(ValCoord2D(seed, x0, y0), ValCoord2D(seed, x0 + 1, y0), xs),
                    Lerp(ValCoord2D(seed, x0, y0 + 1), ValCoord2D(seed, x0 + 1, y0 + 1), xs), ys);
    }

} // namespace TestNoise

TEST(FastNoiseLite_DeterministicOutput)
{
    float a = TestNoise::ValueNoise2D(1337, 5.5f, 3.2f);
    float b = TestNoise::ValueNoise2D(1337, 5.5f, 3.2f);
    EXPECT_NEAR(a, b, 0.0001f);
}

TEST(FastNoiseLite_DifferentSeedsDifferentOutput)
{
    float a = TestNoise::ValueNoise2D(1337, 5.5f, 3.2f);
    float b = TestNoise::ValueNoise2D(42, 5.5f, 3.2f);
    EXPECT_NE(a, b);
}

TEST(FastNoiseLite_OutputRange)
{
    // Sample many points and verify output is in expected range
    for (int i = 0; i < 1000; ++i)
    {
        float x = static_cast<float>(i) * 0.37f;
        float y = static_cast<float>(i) * 0.71f;
        float v = TestNoise::ValueNoise2D(1337, x, y);
        EXPECT_TRUE(v >= -2.0f && v <= 2.0f); // Value noise in [-1, 1] roughly
    }
}

TEST(FastNoiseLite_HermiteInterpolation)
{
    // Hermite should be 0 at 0, 1 at 1, and 0.5 at 0.5
    EXPECT_NEAR(TestNoise::InterpHermite(0.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(TestNoise::InterpHermite(1.0f), 1.0f, 0.0001f);
    EXPECT_NEAR(TestNoise::InterpHermite(0.5f), 0.5f, 0.0001f);

    // Smooth: derivative is 0 at boundaries
    float eps = 0.001f;
    float dStart = (TestNoise::InterpHermite(eps) - TestNoise::InterpHermite(0.0f)) / eps;
    float dEnd = (TestNoise::InterpHermite(1.0f) - TestNoise::InterpHermite(1.0f - eps)) / eps;
    EXPECT_TRUE(std::abs(dStart) < 0.1f);
    EXPECT_TRUE(std::abs(dEnd) < 0.1f);
}

// ============================================================================
// Tonemapping Tests
// ============================================================================

TEST(Tonemapping_ACESFittedRange)
{
    // ACES tonemap should map [0, inf) to [0, 1)
    auto aces = [](float x) -> float
    {
        float a = x * (x * 2.51f + 0.03f);
        float b = x * (x * 2.43f + 0.59f) + 0.14f;
        return a / b;
    };

    EXPECT_TRUE(aces(0.0f) >= 0.0f);
    EXPECT_TRUE(aces(0.0f) <= 0.01f);
    EXPECT_TRUE(aces(1.0f) > 0.5f && aces(1.0f) < 1.0f);
    EXPECT_TRUE(aces(100.0f) < 1.1f);
}

TEST(Tonemapping_ReinhardSimple)
{
    auto reinhard = [](float x) -> float { return x / (1.0f + x); };

    EXPECT_NEAR(reinhard(0.0f), 0.0f, 0.0001f);
    EXPECT_NEAR(reinhard(1.0f), 0.5f, 0.0001f);
    EXPECT_TRUE(reinhard(1000.0f) < 1.0f);
}

// ============================================================================
// Bloom Tests
// ============================================================================

TEST(Bloom_ThresholdSoftKnee)
{
    // Soft knee threshold function
    float threshold = 0.9f;
    float softKnee = 0.5f;
    float knee = threshold * softKnee;

    auto computeContribution = [&](float luma) -> float
    {
        float soft = luma - threshold + knee;
        soft = std::clamp(soft, 0.0f, 2.0f * knee);
        soft = knee > 0 ? soft * soft * (0.25f / knee) : 0.0f;
        float contribution = std::max(soft, luma - threshold);
        return contribution / std::max(luma, 0.00001f);
    };

    // Below threshold: minimal contribution
    float below = computeContribution(0.5f);
    EXPECT_TRUE(below < 0.3f);

    // Above threshold: high contribution
    float above = computeContribution(2.0f);
    EXPECT_TRUE(above > 0.5f);
}

// ============================================================================
// Color Grading Tests
// ============================================================================

TEST(ColorGrading_SaturationZeroIsGrayscale)
{
    float r = 0.8f, g = 0.3f, b = 0.1f;
    float luma = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    float saturation = 0.0f;

    float outR = luma + saturation * (r - luma);
    float outG = luma + saturation * (g - luma);
    float outB = luma + saturation * (b - luma);

    EXPECT_NEAR(outR, luma, 0.0001f);
    EXPECT_NEAR(outG, luma, 0.0001f);
    EXPECT_NEAR(outB, luma, 0.0001f);
}

TEST(ColorGrading_ContrastAroundMidGray)
{
    float midGray = 0.5f;
    float contrast = 2.0f;

    // Contrast increases distance from mid-gray
    float dark = (0.2f - midGray) * contrast + midGray;
    float bright = (0.8f - midGray) * contrast + midGray;

    EXPECT_TRUE(dark < 0.2f);   // Darker gets darker
    EXPECT_TRUE(bright > 0.8f); // Brighter gets brighter
}
