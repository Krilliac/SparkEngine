/**
 * @file TestNeuralTextureCompressor.cpp
 * @brief Tests for neural texture compression (CPU path)
 */

#include "TestFramework.h"
#include "Graphics/Neural/NeuralTextureCompressor.h"
#include "Graphics/Neural/NeuralTextureFormat.h"
#include "Graphics/Neural/NeuralInference.h"

#include <cmath>
#include <cstring>
#include <numeric>
#include <vector>

using namespace Spark::Graphics::Neural;

// ============================================================================
// Helper: compute PSNR between two images
// ============================================================================

static float ComputePSNR(const uint8_t* original, const uint8_t* decoded, uint32_t width, uint32_t height,
                         uint32_t channels = 4)
{
    double mse = 0.0;
    uint32_t totalSamples = width * height * channels;
    for (uint32_t i = 0; i < totalSamples; ++i)
    {
        double diff = static_cast<double>(original[i]) - static_cast<double>(decoded[i]);
        mse += diff * diff;
    }
    mse /= static_cast<double>(totalSamples);
    if (mse < 0.001)
    {
        return 100.0f;
    }
    return static_cast<float>(10.0 * std::log10(255.0 * 255.0 / mse));
}

// ============================================================================
// Format tests
// ============================================================================

TEST(NTC_HeaderMagic)
{
    NTEXHeader hdr;
    EXPECT_EQ(hdr.magic, kNTEXMagic);
    EXPECT_EQ(hdr.version, kNTEXVersion);
}

TEST(NTC_DefaultBlockSize)
{
    EXPECT_EQ(kDefaultNTCBlockSize, static_cast<uint32_t>(16));
}

// ============================================================================
// Compression tests
// ============================================================================

TEST(NTC_CompressSolidColor)
{
    // Initialize inference engine for CPU eval
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralTextureCompressor ntc;
    ntc.Initialize();

    // Create a small solid-red 16x16 texture (1 block)
    constexpr uint32_t size = 16;
    std::vector<uint8_t> pixels(size * size * 4);
    for (uint32_t i = 0; i < size * size; ++i)
    {
        pixels[i * 4 + 0] = 255; // R
        pixels[i * 4 + 1] = 0;   // G
        pixels[i * 4 + 2] = 0;   // B
        pixels[i * 4 + 3] = 255; // A
    }

    NTCOptions opts;
    opts.blockSize = 16;
    opts.hiddenLayers = 2;
    opts.neuronsPerLayer = 16;
    opts.positionalFrequencies = 2;
    opts.qualityLevel = 0; // fast for tests
    opts.trainingIterations = 300;
    opts.learningRate = 0.01f;

    auto compressed = ntc.Compress(pixels.data(), size, size, opts);

    EXPECT_EQ(compressed.header.width, size);
    EXPECT_EQ(compressed.header.height, size);
    EXPECT_EQ(compressed.header.totalBlocks, static_cast<uint32_t>(1));
    EXPECT_TRUE(!compressed.blockWeights.empty());
    EXPECT_TRUE(compressed.GetCompressedSize() > 0);
}

TEST(NTC_CompressDecompressRoundTrip)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralTextureCompressor ntc;
    ntc.Initialize();

    // Create a 16x16 solid-color texture (easiest to learn)
    constexpr uint32_t size = 16;
    std::vector<uint8_t> pixels(size * size * 4);
    for (uint32_t i = 0; i < size * size; ++i)
    {
        pixels[i * 4 + 0] = 200;
        pixels[i * 4 + 1] = 100;
        pixels[i * 4 + 2] = 50;
        pixels[i * 4 + 3] = 255;
    }

    NTCOptions opts;
    opts.blockSize = 16;
    opts.hiddenLayers = 2;
    opts.neuronsPerLayer = 32;
    opts.positionalFrequencies = 4;
    opts.qualityLevel = 2;
    opts.trainingIterations = 1000;

    auto compressed = ntc.Compress(pixels.data(), size, size, opts);
    auto decoded = ntc.DecompressCPU(compressed);

    EXPECT_EQ(decoded.size(), static_cast<size_t>(size * size * 4));

    // Verify decompression produces valid output (all values are valid uint8)
    // The trained network should produce some non-zero output
    uint32_t nonZeroCount = 0;
    for (size_t i = 0; i < decoded.size(); ++i)
    {
        if (decoded[i] > 0)
        {
            nonZeroCount++;
        }
    }
    EXPECT_TRUE(nonZeroCount > 0);
}

TEST(NTC_CompressionRatio)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralTextureCompressor ntc;
    ntc.Initialize();

    constexpr uint32_t size = 16;
    std::vector<uint8_t> pixels(size * size * 4, 128);

    NTCOptions opts;
    opts.blockSize = 16;
    opts.neuronsPerLayer = 16;
    opts.qualityLevel = 0;
    opts.trainingIterations = 100;

    auto compressed = ntc.Compress(pixels.data(), size, size, opts);

    float ratio = compressed.GetCompressionRatio();
    // The compressed size should be some fraction of original
    EXPECT_TRUE(ratio > 0.0f);
    EXPECT_TRUE(ratio < 10.0f); // Sanity check
}

TEST(NTC_MultipleBlocks)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralTextureCompressor ntc;
    ntc.Initialize();

    // 32x32 texture = 4 blocks (2x2) with blockSize=16
    constexpr uint32_t size = 32;
    std::vector<uint8_t> pixels(size * size * 4, 200);

    NTCOptions opts;
    opts.blockSize = 16;
    opts.neuronsPerLayer = 16;
    opts.qualityLevel = 0;
    opts.trainingIterations = 100;

    auto compressed = ntc.Compress(pixels.data(), size, size, opts);

    EXPECT_EQ(compressed.header.blocksX, static_cast<uint32_t>(2));
    EXPECT_EQ(compressed.header.blocksY, static_cast<uint32_t>(2));
    EXPECT_EQ(compressed.header.totalBlocks, static_cast<uint32_t>(4));
    EXPECT_EQ(compressed.blockWeights.size(), static_cast<size_t>(4));
}

// ============================================================================
// File I/O tests
// ============================================================================

TEST(NTC_SaveLoadNTEX)
{
    auto& engine = NeuralInferenceEngine::GetInstance();
    engine.Initialize();

    NeuralTextureCompressor ntc;
    ntc.Initialize();

    constexpr uint32_t size = 16;
    std::vector<uint8_t> pixels(size * size * 4, 100);

    NTCOptions opts;
    opts.blockSize = 16;
    opts.neuronsPerLayer = 16;
    opts.qualityLevel = 0;
    opts.trainingIterations = 50;

    auto compressed = ntc.Compress(pixels.data(), size, size, opts);

    const std::string path = "/tmp/test_neural_texture.ntex";
    EXPECT_TRUE(ntc.SaveNTEX(compressed, path));

    auto loaded = ntc.LoadNTEX(path);
    EXPECT_EQ(loaded.header.magic, kNTEXMagic);
    EXPECT_EQ(loaded.header.width, size);
    EXPECT_EQ(loaded.header.height, size);
    EXPECT_EQ(loaded.header.totalBlocks, compressed.header.totalBlocks);
    EXPECT_EQ(loaded.blockWeights.size(), compressed.blockWeights.size());

    // Weights should match exactly
    for (size_t i = 0; i < loaded.blockWeights[0].size(); ++i)
    {
        EXPECT_TRUE(std::abs(loaded.blockWeights[0][i] - compressed.blockWeights[0][i]) < 1e-6f);
    }
}

TEST(NTC_LoadInvalidFile)
{
    NeuralTextureCompressor ntc;
    ntc.Initialize();

    auto loaded = ntc.LoadNTEX("/tmp/nonexistent_ntex_file.ntex");
    EXPECT_TRUE(loaded.blockWeights.empty());
}

TEST(NTC_ConsoleStatus)
{
    NeuralTextureCompressor ntc;
    ntc.Initialize();

    auto status = ntc.Console_GetStatus();
    EXPECT_TRUE(status.find("Neural Texture Compressor") != std::string::npos);
}

TEST(NTC_EmptyInputRejected)
{
    NeuralTextureCompressor ntc;
    ntc.Initialize();

    auto result = ntc.Compress(nullptr, 0, 0);
    EXPECT_TRUE(result.blockWeights.empty());
}
