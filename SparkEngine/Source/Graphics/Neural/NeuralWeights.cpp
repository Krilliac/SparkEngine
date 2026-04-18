/**
 * @file NeuralWeights.cpp
 * @brief .nnw file I/O for neural network weight serialization
 *
 * Reads v1 (architecture + weights) and v2 (+ optional optimizer state trailer).
 */

#include "NeuralWeights.h"

#include <cstdio>
#include <cstring>

namespace Spark::Graphics::Neural
{

    namespace
    {
        // v1 header was exactly 16 bytes (4 * uint32). v2 adds a flags uint32.
        // We persist the prefix as a 4-uint32 write (v1-compatible on disk)
        // followed by the flags word when version >= 2, and detect the same
        // layout on read via the version field.
        bool ReadHeaderBytes(FILE* file, NNWHeader& header)
        {
            // Read the v1-compatible prefix first.
            uint32_t prefix[4] = {0, 0, 0, 0};
            if (std::fread(prefix, sizeof(uint32_t), 4, file) != 4)
            {
                return false;
            }
            header.magic = prefix[0];
            header.version = prefix[1];
            header.layerCount = prefix[2];
            header.totalParameters = prefix[3];
            header.flags = 0;

            if (header.magic != kNNWMagic)
            {
                return false;
            }
            if (header.version >= 2)
            {
                if (std::fread(&header.flags, sizeof(uint32_t), 1, file) != 1)
                {
                    return false;
                }
            }
            return true;
        }

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
        if (network.desc.layers.empty())
        {
            return false;
        }

        uint32_t totalParams = network.desc.GetTotalParameters();
        if (network.weights.size() != totalParams)
        {
            return false;
        }

        const bool saveOptState =
            network.optimizer.kind != OptimizerKind::None && network.optimizer.firstMoment.size() == totalParams &&
            (network.optimizer.kind != OptimizerKind::Adam || network.optimizer.secondMoment.size() == totalParams);

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

        std::fclose(file);
        return true;
    }

    TrainedNetwork LoadWeights(const std::string& path)
    {
        TrainedNetwork result;

        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file)
        {
            return result;
        }

        NNWHeader header;
        if (!ReadHeaderBytes(file, header))
        {
            std::fclose(file);
            return result;
        }

        // Accept v1 and v2; reject anything higher (forward compat via version bump).
        if (header.version == 0 || header.version > kNNWVersion)
        {
            std::fclose(file);
            return result;
        }

        if (header.layerCount == 0 || header.layerCount > kMaxNetworkLayers)
        {
            std::fclose(file);
            return result;
        }

        result.desc.layers.resize(header.layerCount);
        for (uint32_t i = 0; i < header.layerCount; ++i)
        {
            uint32_t layerData[3];
            if (std::fread(layerData, sizeof(uint32_t), 3, file) != 3)
            {
                std::fclose(file);
                return TrainedNetwork{};
            }
            result.desc.layers[i].inputSize = layerData[0];
            result.desc.layers[i].outputSize = layerData[1];
            result.desc.layers[i].activation = static_cast<ActivationType>(layerData[2]);
        }

        if (result.desc.GetTotalParameters() != header.totalParameters)
        {
            std::fclose(file);
            return TrainedNetwork{};
        }

        result.weights.resize(header.totalParameters);
        if (std::fread(result.weights.data(), sizeof(float), header.totalParameters, file) != header.totalParameters)
        {
            std::fclose(file);
            return TrainedNetwork{};
        }

        if (header.version >= 2 && (header.flags & kNNWFlagOptimizerState) != 0)
        {
            uint32_t trailer[2] = {0, 0};
            if (std::fread(trailer, sizeof(uint32_t), 2, file) != 2)
            {
                std::fclose(file);
                return TrainedNetwork{};
            }
            result.optimizer.kind = static_cast<OptimizerKind>(trailer[0]);
            result.optimizer.stepCount = trailer[1];

            result.optimizer.firstMoment.resize(header.totalParameters);
            if (std::fread(result.optimizer.firstMoment.data(), sizeof(float), header.totalParameters, file) !=
                header.totalParameters)
            {
                std::fclose(file);
                return TrainedNetwork{};
            }
            if (result.optimizer.kind == OptimizerKind::Adam)
            {
                result.optimizer.secondMoment.resize(header.totalParameters);
                if (std::fread(result.optimizer.secondMoment.data(), sizeof(float), header.totalParameters, file) !=
                    header.totalParameters)
                {
                    std::fclose(file);
                    return TrainedNetwork{};
                }
            }
        }

        std::fclose(file);
        return result;
    }

} // namespace Spark::Graphics::Neural
