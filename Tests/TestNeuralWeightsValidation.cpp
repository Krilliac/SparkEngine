/**
 * @file TestNeuralWeightsValidation.cpp
 * @brief Malformed-input and compatibility tests for the production .nnw loader.
 */

#include "TestFramework.h"

#include "Graphics/Neural/NeuralWeights.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace Spark::Graphics::Neural;

namespace
{
    template <typename T> void AppendValue(std::vector<uint8_t>& bytes, const T& value)
    {
        const size_t offset = bytes.size();
        bytes.resize(offset + sizeof(T));
        std::memcpy(bytes.data() + offset, &value, sizeof(T));
    }

    void SetUint32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value)
    {
        ASSERT_TRUE(offset + sizeof(value) <= bytes.size());
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    uint32_t ParameterCount(const std::vector<LayerDesc>& layers)
    {
        uint64_t total = 0;
        for (const LayerDesc& layer : layers)
            total += static_cast<uint64_t>(layer.inputSize) * layer.outputSize + layer.outputSize;
        ASSERT_TRUE(total <= std::numeric_limits<uint32_t>::max());
        return static_cast<uint32_t>(total);
    }

    std::vector<uint8_t> MakeNetworkBytes(uint32_t version, const std::vector<LayerDesc>& layers,
                                          OptimizerKind optimizer = OptimizerKind::None)
    {
        const uint32_t totalParameters = ParameterCount(layers);
        const uint32_t flags = optimizer == OptimizerKind::None ? 0u : kNNWFlagOptimizerState;

        std::vector<uint8_t> bytes;
        AppendValue(bytes, kNNWMagic);
        AppendValue(bytes, version);
        AppendValue(bytes, static_cast<uint32_t>(layers.size()));
        AppendValue(bytes, totalParameters);
        if (version >= 2)
            AppendValue(bytes, flags);

        for (const LayerDesc& layer : layers)
        {
            AppendValue(bytes, layer.inputSize);
            AppendValue(bytes, layer.outputSize);
            AppendValue(bytes, static_cast<uint32_t>(layer.activation));
        }
        for (uint32_t index = 0; index < totalParameters; ++index)
            AppendValue(bytes, static_cast<float>(index) * 0.25f);

        if (optimizer != OptimizerKind::None)
        {
            AppendValue(bytes, static_cast<uint32_t>(optimizer));
            AppendValue(bytes, 17u);
            for (uint32_t index = 0; index < totalParameters; ++index)
                AppendValue(bytes, 0.1f);
            if (optimizer == OptimizerKind::Adam)
            {
                for (uint32_t index = 0; index < totalParameters; ++index)
                    AppendValue(bytes, 0.01f);
            }
        }
        return bytes;
    }

    std::string UniquePath(const char* stem)
    {
        static std::atomic_uint64_t sequence{0};
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        return (std::filesystem::temp_directory_path() /
                (std::string("spark_nnw_") + stem + "_" + std::to_string(tick) + "_" +
                 std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) + ".nnw"))
            .string();
    }

    TrainedNetwork LoadBytes(const std::vector<uint8_t>& bytes, const char* stem)
    {
        const std::string path = UniquePath(stem);
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            ASSERT_TRUE(output.is_open());
            output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ASSERT_TRUE(output.good());
        }
        TrainedNetwork loaded = LoadWeights(path);
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        return loaded;
    }

    bool IsRejected(const std::vector<uint8_t>& bytes, const char* stem)
    {
        const TrainedNetwork loaded = LoadBytes(bytes, stem);
        return loaded.desc.layers.empty() && loaded.weights.empty();
    }
} // namespace

TEST(NNW_ParserAcceptsCanonicalVersionsAndConstantRegressor)
{
    const std::vector<LayerDesc> constantLayers = {{0, 1, ActivationType::None}};
    TrainedNetwork v1 = LoadBytes(MakeNetworkBytes(1, constantLayers), "constant_v1");
    ASSERT_EQ(v1.desc.layers.size(), 1u);
    EXPECT_EQ(v1.desc.layers[0].inputSize, 0u);
    EXPECT_EQ(v1.weights.size(), 1u);

    const std::vector<LayerDesc> adamLayers = {{2, 2, ActivationType::ReLU}, {2, 1, ActivationType::None}};
    TrainedNetwork v2 = LoadBytes(MakeNetworkBytes(2, adamLayers, OptimizerKind::Adam), "adam_v2");
    ASSERT_EQ(v2.desc.layers.size(), 2u);
    EXPECT_EQ(v2.weights.size(), static_cast<size_t>(ParameterCount(adamLayers)));
    EXPECT_TRUE(v2.optimizer.kind == OptimizerKind::Adam);
    EXPECT_EQ(v2.optimizer.firstMoment.size(), v2.weights.size());
    EXPECT_EQ(v2.optimizer.secondMoment.size(), v2.weights.size());

    TrainedNetwork sgd = LoadBytes(MakeNetworkBytes(2, adamLayers, OptimizerKind::SGDMomentum), "sgd_v2");
    ASSERT_EQ(sgd.desc.layers.size(), 2u);
    EXPECT_TRUE(sgd.optimizer.kind == OptimizerKind::SGDMomentum);
    EXPECT_EQ(sgd.optimizer.firstMoment.size(), sgd.weights.size());
    EXPECT_TRUE(sgd.optimizer.secondMoment.empty());
}

TEST(NNW_ParserRejectsUnknownFlagsAndTrailingData)
{
    std::vector<uint8_t> unknownFlags = MakeNetworkBytes(2, {{2, 2, ActivationType::ReLU}});
    SetUint32(unknownFlags, 16, 0x80000000u);
    EXPECT_TRUE(IsRejected(unknownFlags, "unknown_flags"));

    std::vector<uint8_t> trailing = MakeNetworkBytes(2, {{2, 2, ActivationType::ReLU}});
    trailing.push_back(0x7fu);
    EXPECT_TRUE(IsRejected(trailing, "trailing"));
}

TEST(NNW_ParserRejectsInvalidLayerMetadata)
{
    EXPECT_TRUE(IsRejected(MakeNetworkBytes(2, {{2, 257, ActivationType::ReLU}}), "wide_layer"));
    EXPECT_TRUE(
        IsRejected(MakeNetworkBytes(2, {{2, 2, ActivationType::ReLU}, {3, 1, ActivationType::None}}), "disconnected"));

    std::vector<uint8_t> invalidActivation = MakeNetworkBytes(2, {{2, 2, ActivationType::ReLU}});
    SetUint32(invalidActivation, 28, 256u);
    EXPECT_TRUE(IsRejected(invalidActivation, "activation_narrowing"));

    std::vector<uint8_t> wrapped;
    AppendValue(wrapped, kNNWMagic);
    AppendValue(wrapped, 2u);
    AppendValue(wrapped, 1u);
    AppendValue(wrapped, 0u);
    AppendValue(wrapped, 0u);
    AppendValue(wrapped, std::numeric_limits<uint32_t>::max());
    AppendValue(wrapped, 1u);
    AppendValue(wrapped, static_cast<uint32_t>(ActivationType::None));
    EXPECT_TRUE(IsRejected(wrapped, "wrapped_parameters"));
}

TEST(NNW_ParserRejectsInvalidOptimizerTrailer)
{
    const std::vector<LayerDesc> layers = {{2, 1, ActivationType::None}};
    std::vector<uint8_t> unknownKind = MakeNetworkBytes(2, layers, OptimizerKind::Adam);
    const size_t trailerOffset = 20 + layers.size() * 12 + ParameterCount(layers) * sizeof(float);
    SetUint32(unknownKind, trailerOffset, 99u);
    EXPECT_TRUE(IsRejected(unknownKind, "unknown_optimizer"));

    std::vector<uint8_t> noneWithTrailer = MakeNetworkBytes(2, layers, OptimizerKind::Adam);
    SetUint32(noneWithTrailer, trailerOffset, static_cast<uint32_t>(OptimizerKind::None));
    EXPECT_TRUE(IsRejected(noneWithTrailer, "none_optimizer"));

    std::vector<uint8_t> truncated = MakeNetworkBytes(2, layers, OptimizerKind::Adam);
    truncated.pop_back();
    EXPECT_TRUE(IsRejected(truncated, "truncated_optimizer"));
}

TEST(NNW_ParserAcceptsMaximumAdamAndRejectsOversizedFile)
{
    const std::vector<LayerDesc> maximumLayers(8, {256, 256, ActivationType::ReLU});
    const std::vector<uint8_t> maximum = MakeNetworkBytes(2, maximumLayers, OptimizerKind::Adam);
    EXPECT_EQ(maximum.size(), static_cast<size_t>(6316156));

    TrainedNetwork loaded = LoadBytes(maximum, "maximum_adam");
    ASSERT_EQ(loaded.desc.layers.size(), 8u);
    EXPECT_EQ(loaded.weights.size(), static_cast<size_t>(526336));
    EXPECT_EQ(loaded.optimizer.firstMoment.size(), loaded.weights.size());
    EXPECT_EQ(loaded.optimizer.secondMoment.size(), loaded.weights.size());

    std::vector<uint8_t> oversized(6316157, 0u);
    EXPECT_TRUE(IsRejected(oversized, "oversized_file"));
}

TEST(NNW_ParserSaverRejectsUnreloadableNetworks)
{
    const auto expectSaveRejected = [](TrainedNetwork network, const char* stem)
    {
        const std::string path = UniquePath(stem);
        EXPECT_FALSE(SaveWeights(network, path));
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    };

    TrainedNetwork disconnected;
    disconnected.desc.layers = {{2, 2, ActivationType::ReLU}, {3, 1, ActivationType::None}};
    disconnected.weights.assign(ParameterCount(disconnected.desc.layers), 0.0f);
    expectSaveRejected(disconnected, "save_disconnected");

    TrainedNetwork tooWide;
    tooWide.desc.layers = {{2, 257, ActivationType::None}};
    tooWide.weights.assign(ParameterCount(tooWide.desc.layers), 0.0f);
    expectSaveRejected(tooWide, "save_too_wide");

    TrainedNetwork unknownOptimizer;
    unknownOptimizer.desc.layers = {{1, 1, ActivationType::None}};
    unknownOptimizer.weights.assign(ParameterCount(unknownOptimizer.desc.layers), 0.0f);
    unknownOptimizer.optimizer.kind = static_cast<OptimizerKind>(99u);
    unknownOptimizer.optimizer.firstMoment.assign(unknownOptimizer.weights.size(), 0.0f);
    expectSaveRejected(unknownOptimizer, "save_unknown_optimizer");

    TrainedNetwork incompleteAdam;
    incompleteAdam.desc.layers = {{1, 1, ActivationType::None}};
    incompleteAdam.weights.assign(ParameterCount(incompleteAdam.desc.layers), 0.0f);
    incompleteAdam.optimizer.kind = OptimizerKind::Adam;
    incompleteAdam.optimizer.firstMoment.assign(incompleteAdam.weights.size(), 0.0f);
    expectSaveRejected(incompleteAdam, "save_incomplete_adam");
}
