/**
 * @file NeuralWeights.cpp
 * @brief .nnw file I/O for neural network weight serialization
 *
 * Reads v1 (architecture + weights) and v2 (+ optional optimizer state trailer).
 */

#include "NeuralWeights.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

namespace Spark::Graphics::Neural
{

    namespace
    {
        constexpr size_t kNNWV1HeaderBytes = sizeof(uint32_t) * 4;
        constexpr size_t kNNWV2HeaderBytes = sizeof(uint32_t) * 5;
        constexpr size_t kNNWLayerBytes = sizeof(uint32_t) * 3;
        constexpr size_t kNNWOptimizerHeaderBytes = sizeof(uint32_t) * 2;
        constexpr uint32_t kNNWKnownFlags = kNNWFlagOptimizerState;
        constexpr uint64_t kNNWMaxParameters =
            static_cast<uint64_t>(kMaxNetworkLayers) *
            (static_cast<uint64_t>(kMaxNeuronsPerLayer) * kMaxNeuronsPerLayer + kMaxNeuronsPerLayer);
        constexpr size_t kNNWMaxFileBytes =
            kNNWV2HeaderBytes + static_cast<size_t>(kMaxNetworkLayers) * kNNWLayerBytes +
            static_cast<size_t>(kNNWMaxParameters) * sizeof(float) * 3 + kNNWOptimizerHeaderBytes;

        static_assert(sizeof(float) == 4, ".nnw requires 32-bit IEEE float storage");
        static_assert(kNNWMaxParameters == 526336);
        static_assert(kNNWMaxFileBytes == 6316156);

        class ByteReader
        {
          public:
            explicit ByteReader(std::span<const uint8_t> bytes) : m_bytes(bytes) {}

            template <typename T> bool Read(T& value)
            {
                static_assert(std::is_trivially_copyable_v<T>);
                if (Remaining() < sizeof(T))
                    return false;
                std::memcpy(&value, m_bytes.data() + m_offset, sizeof(T));
                m_offset += sizeof(T);
                return true;
            }

            bool ReadFloats(std::vector<float>& values, uint32_t count)
            {
                const size_t bytes = static_cast<size_t>(count) * sizeof(float);
                if (Remaining() < bytes)
                    return false;
                values.resize(count);
                if (bytes != 0)
                    std::memcpy(values.data(), m_bytes.data() + m_offset, bytes);
                m_offset += bytes;
                return true;
            }

            bool PeekUint32(size_t relativeOffset, uint32_t& value) const
            {
                if (relativeOffset > Remaining() || Remaining() - relativeOffset < sizeof(value))
                    return false;
                std::memcpy(&value, m_bytes.data() + m_offset + relativeOffset, sizeof(value));
                return true;
            }

            size_t Remaining() const { return m_bytes.size() - m_offset; }

          private:
            std::span<const uint8_t> m_bytes;
            size_t m_offset = 0;
        };

        bool ValidateDescription(const std::vector<LayerDesc>& layers, uint32_t& totalParameters)
        {
            if (layers.empty() || layers.size() > kMaxNetworkLayers)
                return false;

            uint64_t total = 0;
            uint32_t priorOutput = 0;
            for (size_t index = 0; index < layers.size(); ++index)
            {
                const LayerDesc& layer = layers[index];
                if (layer.inputSize > kMaxNeuronsPerLayer || layer.outputSize > kMaxNeuronsPerLayer)
                    return false;
                if (static_cast<uint32_t>(layer.activation) > static_cast<uint32_t>(ActivationType::None))
                    return false;
                if (index != 0 && layer.inputSize != priorOutput)
                    return false;

                total += static_cast<uint64_t>(layer.inputSize) * layer.outputSize + layer.outputSize;
                if (total > kNNWMaxParameters || total > std::numeric_limits<uint32_t>::max())
                    return false;
                priorOutput = layer.outputSize;
            }
            totalParameters = static_cast<uint32_t>(total);
            return true;
        }

        TrainedNetwork ParseWeights(std::span<const uint8_t> bytes)
        {
            TrainedNetwork result;
            if (bytes.size() < kNNWV1HeaderBytes || bytes.size() > kNNWMaxFileBytes)
                return result;

            ByteReader reader(bytes);
            uint32_t magic = 0;
            uint32_t version = 0;
            uint32_t layerCount = 0;
            uint32_t declaredParameters = 0;
            uint32_t flags = 0;
            if (!reader.Read(magic) || !reader.Read(version) || !reader.Read(layerCount) ||
                !reader.Read(declaredParameters))
            {
                return {};
            }
            if (magic != kNNWMagic || version == 0 || version > kNNWVersion)
                return {};
            if (version >= 2 && !reader.Read(flags))
                return {};
            if ((flags & ~kNNWKnownFlags) != 0 || layerCount == 0 || layerCount > kMaxNetworkLayers)
                return {};

            result.desc.layers.reserve(layerCount);
            for (uint32_t index = 0; index < layerCount; ++index)
            {
                uint32_t inputSize = 0;
                uint32_t outputSize = 0;
                uint32_t activation = 0;
                if (!reader.Read(inputSize) || !reader.Read(outputSize) || !reader.Read(activation))
                    return {};
                if (inputSize > kMaxNeuronsPerLayer || outputSize > kMaxNeuronsPerLayer ||
                    activation > static_cast<uint32_t>(ActivationType::None))
                {
                    return {};
                }
                result.desc.layers.push_back({inputSize, outputSize, static_cast<ActivationType>(activation)});
            }

            uint32_t validatedParameters = 0;
            if (!ValidateDescription(result.desc.layers, validatedParameters) ||
                validatedParameters != declaredParameters)
            {
                return {};
            }

            const size_t weightsBytes = static_cast<size_t>(validatedParameters) * sizeof(float);
            size_t expectedRemaining = weightsBytes;
            OptimizerKind optimizerKind = OptimizerKind::None;
            if ((flags & kNNWFlagOptimizerState) != 0)
            {
                uint32_t rawKind = 0;
                if (!reader.PeekUint32(weightsBytes, rawKind))
                    return {};
                if (rawKind == static_cast<uint32_t>(OptimizerKind::Adam))
                    optimizerKind = OptimizerKind::Adam;
                else if (rawKind == static_cast<uint32_t>(OptimizerKind::SGDMomentum))
                    optimizerKind = OptimizerKind::SGDMomentum;
                else
                    return {};

                const size_t momentCount = optimizerKind == OptimizerKind::Adam ? 2u : 1u;
                expectedRemaining += kNNWOptimizerHeaderBytes + weightsBytes * momentCount;
            }
            if (reader.Remaining() != expectedRemaining || !reader.ReadFloats(result.weights, validatedParameters))
                return {};

            if (optimizerKind != OptimizerKind::None)
            {
                uint32_t rawKind = 0;
                if (!reader.Read(rawKind) || !reader.Read(result.optimizer.stepCount))
                    return {};
                result.optimizer.kind = static_cast<OptimizerKind>(rawKind);
                if (!reader.ReadFloats(result.optimizer.firstMoment, validatedParameters))
                    return {};
                if (optimizerKind == OptimizerKind::Adam &&
                    !reader.ReadFloats(result.optimizer.secondMoment, validatedParameters))
                {
                    return {};
                }
            }
            return reader.Remaining() == 0 ? result : TrainedNetwork{};
        }

        // v1 header was exactly 16 bytes (4 * uint32). v2 adds a flags uint32.
        // We persist the prefix as a 4-uint32 write (v1-compatible on disk)
        // followed by the flags word when version >= 2, and detect the same
        // layout on read via the version field.
        bool WriteHeaderBytes(FILE* file, const NNWHeader& header)
        {
            uint32_t prefix[4] = {header.magic, header.version, header.layerCount, header.totalParameters};
            if (std::fwrite(prefix, sizeof(uint32_t), 4, file) != 4)
            {
                return false;
            }
            if (header.version >= 2)
            {
                if (std::fwrite(&header.flags, sizeof(uint32_t), 1, file) != 1)
                {
                    return false;
                }
            }
            return true;
        }
    } // namespace

    bool SaveWeights(const TrainedNetwork& network, const std::string& path)
    {
        uint32_t totalParams = 0;
        if (!ValidateDescription(network.desc.layers, totalParams) || network.weights.size() != totalParams)
            return false;

        bool saveOptState = false;
        switch (network.optimizer.kind)
        {
        case OptimizerKind::None:
            break;
        case OptimizerKind::Adam:
            if (network.optimizer.firstMoment.size() != totalParams ||
                network.optimizer.secondMoment.size() != totalParams)
                return false;
            saveOptState = true;
            break;
        case OptimizerKind::SGDMomentum:
            if (network.optimizer.firstMoment.size() != totalParams)
                return false;
            saveOptState = true;
            break;
        default:
            return false;
        }

        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file)
        {
            return false;
        }

        NNWHeader header;
        header.layerCount = static_cast<uint32_t>(network.desc.layers.size());
        header.totalParameters = totalParams;
        header.flags = saveOptState ? kNNWFlagOptimizerState : 0u;

        if (!WriteHeaderBytes(file, header))
        {
            std::fclose(file);
            return false;
        }

        for (const auto& layer : network.desc.layers)
        {
            uint32_t layerData[3] = {layer.inputSize, layer.outputSize, static_cast<uint32_t>(layer.activation)};
            if (std::fwrite(layerData, sizeof(uint32_t), 3, file) != 3)
            {
                std::fclose(file);
                return false;
            }
        }

        if (std::fwrite(network.weights.data(), sizeof(float), totalParams, file) != totalParams)
        {
            std::fclose(file);
            return false;
        }

        if (saveOptState)
        {
            uint32_t trailer[2] = {static_cast<uint32_t>(network.optimizer.kind), network.optimizer.stepCount};
            if (std::fwrite(trailer, sizeof(uint32_t), 2, file) != 2)
            {
                std::fclose(file);
                return false;
            }
            if (std::fwrite(network.optimizer.firstMoment.data(), sizeof(float), totalParams, file) != totalParams)
            {
                std::fclose(file);
                return false;
            }
            if (network.optimizer.kind == OptimizerKind::Adam)
            {
                if (std::fwrite(network.optimizer.secondMoment.data(), sizeof(float), totalParams, file) != totalParams)
                {
                    std::fclose(file);
                    return false;
                }
            }
        }

        return std::fclose(file) == 0;
    }

    TrainedNetwork LoadWeights(const std::string& path)
    {
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file)
            return {};

        if (std::fseek(file, 0, SEEK_END) != 0)
        {
            std::fclose(file);
            return {};
        }
        const long rawSize = std::ftell(file);
        if (rawSize < 0 || static_cast<unsigned long long>(rawSize) > kNNWMaxFileBytes ||
            std::fseek(file, 0, SEEK_SET) != 0)
        {
            std::fclose(file);
            return {};
        }

        std::vector<uint8_t> bytes(static_cast<size_t>(rawSize));
        if ((!bytes.empty() && std::fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) ||
            std::fgetc(file) != EOF || std::ferror(file) != 0)
        {
            std::fclose(file);
            return {};
        }
        std::fclose(file);
        return ParseWeights(bytes);
    }

} // namespace Spark::Graphics::Neural
