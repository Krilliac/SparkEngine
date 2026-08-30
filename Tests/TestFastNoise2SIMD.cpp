/**
 * @file TestFastNoise2SIMD.cpp
 * @brief Phase S — CPU reference tests for FastNoise2SIMD
 *
 * Exercises the header-only `Spark::Graphics::FastNoise2SIMD` family
 * directly: `SimplexNode`, `PerlinNode`, `CellularNode`, `FBMNode`,
 * `DomainWarpNode`, `CombinerNode`, and the owning `NoiseGraph`.
 * All nodes are pure CPU (runtime SIMD detection picks the best
 * available kernel, with a scalar fallback), so every test runs on
 * every platform's CI.
 *
 * Phase S also wires a default `NoiseGraph` as a `unique_ptr` member
 * of `Spark::Graphics::GraphicsEngine`. These tests pin the direct
 * class contract; the engine-side lifecycle is exercised by
 * integration tests in `TestGraphicsEnginePhaseS.cpp` (if added in a
 * follow-up).
 */

#include "TestFramework.h"

#include "Graphics/FastNoise2SIMD.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

using Spark::Graphics::CellularNode;
using Spark::Graphics::CombineOp;
using Spark::Graphics::CombinerNode;
using Spark::Graphics::DetectBestSIMD;
using Spark::Graphics::DomainWarpNode;
using Spark::Graphics::FBMNode;
using Spark::Graphics::NoiseGraph;
using Spark::Graphics::NoiseNodeType;
using Spark::Graphics::PerlinNode;
using Spark::Graphics::SIMDLevel;
using Spark::Graphics::SIMDLevelName;
using Spark::Graphics::SimplexNode;

// =========================================================================
// SIMD detection
// =========================================================================

TEST(FastNoise2SIMD_DetectBestSIMDReturnsValidLevel)
{
    SIMDLevel level = DetectBestSIMD();
    int iv = static_cast<int>(level);
    // Valid range: 0 (Scalar) .. 2 (AVX2)
    EXPECT_GE(iv, 0);
    EXPECT_LE(iv, 2);
}

TEST(FastNoise2SIMD_SIMDLevelNameIsNonNull)
{
    EXPECT_TRUE(SIMDLevelName(SIMDLevel::Scalar) != nullptr);
    EXPECT_TRUE(SIMDLevelName(SIMDLevel::SSE2) != nullptr);
    EXPECT_TRUE(SIMDLevelName(SIMDLevel::AVX2) != nullptr);
    // Out-of-range falls through to "Unknown".
    EXPECT_TRUE(SIMDLevelName(static_cast<SIMDLevel>(99)) != nullptr);
}

TEST(FastNoise2SIMD_NoiseGraphReportsSIMDLevel)
{
    SIMDLevel graphLevel = NoiseGraph::GetSIMDLevel();
    SIMDLevel directLevel = DetectBestSIMD();
    EXPECT_EQ(static_cast<int>(graphLevel), static_cast<int>(directLevel));
}

TEST(FastNoise2SIMD_HashVectorsUseDefinedUnsignedWraparound)
{
    using Spark::Graphics::NoiseDetail::Hash;

    EXPECT_EQ(Hash(0, 0, 0), uint32_t{0x00000000u});
    EXPECT_EQ(Hash(1, 2, 3), uint32_t{0x141ee43eu});
    EXPECT_EQ(Hash(-1, -2, -3), uint32_t{0x00a90e16u});
    EXPECT_EQ(Hash(2147483647, (-2147483647 - 1), 1337), uint32_t{0x71b69af6u});
    EXPECT_EQ(Hash(-123456789, 987654321, -2024), uint32_t{0x60469bc9u});
}

// =========================================================================
// SimplexNode
// =========================================================================

TEST(FastNoise2SIMD_SimplexNodeProducesBoundedOutput)
{
    SimplexNode node;
    node.SetSeed(1234);
    node.SetFrequency(0.05f);

    // Sample an 8x8 grid and check the output lies within a
    // reasonable range. Simplex noise is nominally in [-1, 1] but
    // the raw scalar path multiplies by 70 before returning, so it
    // can exceed that briefly — we assert |v| <= 2 as a sanity bound.
    for (int y = 0; y < 8; ++y)
    {
        for (int x = 0; x < 8; ++x)
        {
            float v = node.Evaluate(static_cast<float>(x), static_cast<float>(y));
            EXPECT_LE(std::fabs(v), 2.0f);
        }
    }
}

TEST(FastNoise2SIMD_SimplexNodeIsDeterministic)
{
    SimplexNode node;
    node.SetSeed(42);
    node.SetFrequency(0.1f);

    // Same input → same output across repeated calls.
    float a = node.Evaluate(1.5f, 2.5f);
    float b = node.Evaluate(1.5f, 2.5f);
    EXPECT_NEAR(a, b, 1e-6f);
}

TEST(FastNoise2SIMD_SimplexNodeDifferentSeedsDiffer)
{
    SimplexNode a;
    a.SetSeed(1);
    a.SetFrequency(0.1f);
    SimplexNode b;
    b.SetSeed(2);
    b.SetFrequency(0.1f);

    // Different seeds should produce different values at the same point.
    bool foundDifference = false;
    for (int i = 0; i < 8 && !foundDifference; ++i)
    {
        float va = a.Evaluate(static_cast<float>(i) * 0.5f, static_cast<float>(i) * 0.3f);
        float vb = b.Evaluate(static_cast<float>(i) * 0.5f, static_cast<float>(i) * 0.3f);
        if (std::fabs(va - vb) > 1e-4f)
        {
            foundDifference = true;
        }
    }
    EXPECT_TRUE(foundDifference);
}

TEST(FastNoise2SIMD_SimplexNodeTypeIsSimplex)
{
    SimplexNode node;
    EXPECT_EQ(static_cast<int>(node.GetType()), static_cast<int>(NoiseNodeType::Simplex));
}

// =========================================================================
// PerlinNode
// =========================================================================

TEST(FastNoise2SIMD_PerlinNodeIsDeterministic)
{
    PerlinNode node;
    node.SetSeed(7);
    node.SetFrequency(0.1f);
    float a = node.Evaluate(3.14f, 2.71f);
    float b = node.Evaluate(3.14f, 2.71f);
    EXPECT_NEAR(a, b, 1e-6f);
}

TEST(FastNoise2SIMD_PerlinNodeTypeIsPerlin)
{
    PerlinNode node;
    EXPECT_EQ(static_cast<int>(node.GetType()), static_cast<int>(NoiseNodeType::Perlin));
}

// =========================================================================
// CellularNode
// =========================================================================

TEST(FastNoise2SIMD_CellularNodeIsDeterministic)
{
    CellularNode node;
    node.SetSeed(13);
    node.SetFrequency(0.2f);
    float a = node.Evaluate(5.0f, 5.0f);
    float b = node.Evaluate(5.0f, 5.0f);
    EXPECT_NEAR(a, b, 1e-6f);
}

TEST(FastNoise2SIMD_CellularNodeTypeIsCellular)
{
    CellularNode node;
    EXPECT_EQ(static_cast<int>(node.GetType()), static_cast<int>(NoiseNodeType::Cellular));
}

TEST(FastNoise2SIMD_CellularGoldenValuesPreserveLegacyHashing)
{
    CellularNode node;
    node.SetSeed(13);
    node.SetFrequency(0.2f);
    EXPECT_NEAR(node.Evaluate(5.0f, 5.0f), 0.1561257094f, 1e-6f);

    node.SetSeed(-2024);
    node.SetFrequency(0.23f);
    EXPECT_NEAR(node.Evaluate(-6.75f, 6.125f), 0.2805069685f, 1e-6f);

    node.SetSeed(0x13579BDF);
    node.SetFrequency(0.03125f);
    EXPECT_NEAR(node.Evaluate(123.5f, -87.25f), 0.1003850102f, 1e-6f);
}

// =========================================================================
// FBMNode
// =========================================================================

TEST(FastNoise2SIMD_FBMNodeHonoursOctaves)
{
    SimplexNode child;
    child.SetSeed(1337);
    child.SetFrequency(0.05f);

    FBMNode fbm(&child, /*octaves*/ 4, /*lacunarity*/ 2.0f, /*gain*/ 0.5f);
    EXPECT_EQ(fbm.GetOctaves(), 4);
    EXPECT_NEAR(fbm.GetLacunarity(), 2.0f, 1e-6f);
    EXPECT_NEAR(fbm.GetGain(), 0.5f, 1e-6f);
}

TEST(FastNoise2SIMD_FBMNodeIsDeterministic)
{
    SimplexNode child;
    child.SetSeed(2024);
    child.SetFrequency(0.05f);
    FBMNode fbm(&child, 3, 2.0f, 0.5f);

    float a = fbm.Evaluate(10.0f, 15.0f);
    float b = fbm.Evaluate(10.0f, 15.0f);
    EXPECT_NEAR(a, b, 1e-6f);
}

TEST(FastNoise2SIMD_FBMNodeTypeIsFBM)
{
    SimplexNode child;
    FBMNode fbm(&child);
    EXPECT_EQ(static_cast<int>(fbm.GetType()), static_cast<int>(NoiseNodeType::FBM));
}

// =========================================================================
// DomainWarpNode
// =========================================================================

TEST(FastNoise2SIMD_DomainWarpAmplitudeAccessors)
{
    SimplexNode source;
    SimplexNode warp;
    DomainWarpNode warpNode(&source, &warp, 2.5f);
    EXPECT_NEAR(warpNode.GetAmplitude(), 2.5f, 1e-6f);
    warpNode.SetAmplitude(4.0f);
    EXPECT_NEAR(warpNode.GetAmplitude(), 4.0f, 1e-6f);
}

TEST(FastNoise2SIMD_DomainWarpTypeIsDomainWarp)
{
    SimplexNode source;
    SimplexNode warp;
    DomainWarpNode warpNode(&source, &warp, 1.0f);
    EXPECT_EQ(static_cast<int>(warpNode.GetType()), static_cast<int>(NoiseNodeType::DomainWarp));
}

TEST(FastNoise2SIMD_DomainWarpNullChildIsSafe)
{
    DomainWarpNode warpNode(nullptr, nullptr, 1.0f);
    EXPECT_NEAR(warpNode.Evaluate(1.0f, 1.0f), 0.0f, 1e-6f);
}

// =========================================================================
// CombinerNode
// =========================================================================

TEST(FastNoise2SIMD_CombinerAddOp)
{
    SimplexNode a;
    a.SetSeed(1);
    a.SetFrequency(0.1f);
    SimplexNode b;
    b.SetSeed(2);
    b.SetFrequency(0.1f);

    CombinerNode combiner(&a, &b, CombineOp::Add);
    EXPECT_EQ(static_cast<int>(combiner.GetOp()), static_cast<int>(CombineOp::Add));

    float expected = a.Evaluate(3.0f, 4.0f) + b.Evaluate(3.0f, 4.0f);
    EXPECT_NEAR(combiner.Evaluate(3.0f, 4.0f), expected, 1e-4f);
}

TEST(FastNoise2SIMD_CombinerMultiplyOp)
{
    SimplexNode a;
    SimplexNode b;
    a.SetSeed(10);
    b.SetSeed(20);
    a.SetFrequency(0.2f);
    b.SetFrequency(0.2f);
    CombinerNode combiner(&a, &b, CombineOp::Multiply);

    float expected = a.Evaluate(1.0f, 2.0f) * b.Evaluate(1.0f, 2.0f);
    EXPECT_NEAR(combiner.Evaluate(1.0f, 2.0f), expected, 1e-4f);
}

TEST(FastNoise2SIMD_CombinerMinMaxOps)
{
    SimplexNode a;
    SimplexNode b;
    a.SetSeed(100);
    b.SetSeed(200);
    a.SetFrequency(0.05f);
    b.SetFrequency(0.05f);

    CombinerNode minC(&a, &b, CombineOp::Min);
    CombinerNode maxC(&a, &b, CombineOp::Max);

    float va = a.Evaluate(5.0f, 6.0f);
    float vb = b.Evaluate(5.0f, 6.0f);
    EXPECT_NEAR(minC.Evaluate(5.0f, 6.0f), std::min(va, vb), 1e-4f);
    EXPECT_NEAR(maxC.Evaluate(5.0f, 6.0f), std::max(va, vb), 1e-4f);
}

TEST(FastNoise2SIMD_CombinerNullChildrenIsSafe)
{
    CombinerNode combiner(nullptr, nullptr, CombineOp::Add);
    EXPECT_NEAR(combiner.Evaluate(1.0f, 1.0f), 0.0f, 1e-6f);
}

TEST(FastNoise2SIMD_CombinerSetOp)
{
    SimplexNode a;
    SimplexNode b;
    CombinerNode combiner(&a, &b, CombineOp::Add);
    combiner.SetOp(CombineOp::Max);
    EXPECT_EQ(static_cast<int>(combiner.GetOp()), static_cast<int>(CombineOp::Max));
}

// =========================================================================
// Batch evaluation consistency with single-sample evaluation
// =========================================================================

TEST(FastNoise2SIMD_BatchEvaluateMatchesSingleEvaluate)
{
    SimplexNode node;
    node.SetSeed(999);
    node.SetFrequency(0.03f);

    constexpr int count = 13; // non-multiple of 4 to exercise the remainder loop
    std::vector<float> inX(count), inY(count), out(count);
    for (int i = 0; i < count; ++i)
    {
        inX[i] = static_cast<float>(i) * 0.7f;
        inY[i] = static_cast<float>(i) * 0.3f;
    }
    node.BatchEvaluate(out.data(), inX.data(), inY.data(), count);

    for (int i = 0; i < count; ++i)
    {
        float expected = node.Evaluate(inX[i], inY[i]);
        EXPECT_NEAR(out[i], expected, 1e-4f);
    }
}

TEST(FastNoise2SIMD_PerlinAndCellularBatchMatchSingleEvaluate)
{
    constexpr int count = 13;
    std::vector<float> inX(count), inY(count), out(count);
    for (int i = 0; i < count; ++i)
    {
        inX[i] = static_cast<float>(i - 6) * 1.125f;
        inY[i] = static_cast<float>(7 - i) * 0.875f;
    }

    PerlinNode perlin;
    perlin.SetSeed(-2024);
    perlin.SetFrequency(0.17f);
    perlin.BatchEvaluate(out.data(), inX.data(), inY.data(), count);
    for (int i = 0; i < count; ++i)
        EXPECT_NEAR(out[i], perlin.Evaluate(inX[i], inY[i]), 1e-6f);

    CellularNode cellular;
    cellular.SetSeed(0x13579BDF);
    cellular.SetFrequency(0.23f);
    cellular.BatchEvaluate(out.data(), inX.data(), inY.data(), count);
    for (int i = 0; i < count; ++i)
        EXPECT_NEAR(out[i], cellular.Evaluate(inX[i], inY[i]), 1e-6f);
}

TEST(FastNoise2SIMD_FBMBatchMatchesSingle)
{
    SimplexNode child;
    child.SetSeed(555);
    child.SetFrequency(0.02f);
    FBMNode fbm(&child, 3, 2.0f, 0.5f);

    constexpr int count = 8;
    std::vector<float> inX(count), inY(count), out(count);
    for (int i = 0; i < count; ++i)
    {
        inX[i] = static_cast<float>(i);
        inY[i] = static_cast<float>(i) * 2.0f;
    }
    fbm.BatchEvaluate(out.data(), inX.data(), inY.data(), count);

    // FBM batch normalises per-call; tolerate a slightly larger
    // difference because of the mul-add ordering with maxAmplitude
    // normalisation.
    for (int i = 0; i < count; ++i)
    {
        float expected = fbm.Evaluate(inX[i], inY[i]);
        EXPECT_NEAR(out[i], expected, 1e-3f);
    }
}

// =========================================================================
// NoiseGraph ownership + routing
// =========================================================================

TEST(FastNoise2SIMD_NoiseGraphEmptyEvaluatesZero)
{
    NoiseGraph graph;
    EXPECT_EQ(graph.GetNodeCount(), 0);
    EXPECT_TRUE(graph.GetOutputNode() == nullptr);
    EXPECT_NEAR(graph.Evaluate(1.0f, 2.0f), 0.0f, 1e-6f);
}

TEST(FastNoise2SIMD_NoiseGraphAddsNodeAndRoutes)
{
    NoiseGraph graph;
    auto node = std::make_unique<SimplexNode>();
    node->SetSeed(321);
    node->SetFrequency(0.05f);

    auto* raw = graph.AddNode(std::move(node));
    EXPECT_TRUE(raw != nullptr);
    EXPECT_EQ(graph.GetNodeCount(), 1);

    graph.SetOutputNode(raw);
    EXPECT_EQ(graph.GetOutputNode(), raw);

    float direct = raw->Evaluate(2.0f, 3.0f);
    float viaGraph = graph.Evaluate(2.0f, 3.0f);
    EXPECT_NEAR(viaGraph, direct, 1e-6f);
}

TEST(FastNoise2SIMD_NoiseGraphBatchEvaluateWithoutOutputIsZero)
{
    NoiseGraph graph;
    // No output set — BatchEvaluate must zero-fill the output buffer.
    std::vector<float> inX{1.0f, 2.0f, 3.0f, 4.0f};
    std::vector<float> inY{5.0f, 6.0f, 7.0f, 8.0f};
    std::vector<float> out{99.0f, 99.0f, 99.0f, 99.0f};
    graph.BatchEvaluate(out.data(), inX.data(), inY.data(), 4);
    for (float v : out)
    {
        EXPECT_NEAR(v, 0.0f, 1e-6f);
    }
}

TEST(FastNoise2SIMD_NoiseGraphBatchMatchesOutputNode)
{
    NoiseGraph graph;
    auto* node = graph.AddNode(std::make_unique<SimplexNode>());
    node->SetSeed(42);
    node->SetFrequency(0.1f);
    graph.SetOutputNode(node);

    constexpr int count = 6;
    std::vector<float> inX(count), inY(count), graphOut(count), nodeOut(count);
    for (int i = 0; i < count; ++i)
    {
        inX[i] = static_cast<float>(i) * 0.5f;
        inY[i] = static_cast<float>(i) * 0.4f;
    }
    graph.BatchEvaluate(graphOut.data(), inX.data(), inY.data(), count);
    node->BatchEvaluate(nodeOut.data(), inX.data(), inY.data(), count);

    for (int i = 0; i < count; ++i)
    {
        EXPECT_NEAR(graphOut[i], nodeOut[i], 1e-6f);
    }
}

TEST(FastNoise2SIMD_NoiseGraphOwnsMultipleNodes)
{
    NoiseGraph graph;
    graph.AddNode(std::make_unique<SimplexNode>());
    graph.AddNode(std::make_unique<PerlinNode>());
    graph.AddNode(std::make_unique<CellularNode>());
    EXPECT_EQ(graph.GetNodeCount(), 3);
}
