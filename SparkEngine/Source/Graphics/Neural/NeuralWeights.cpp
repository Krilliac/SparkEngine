/**
 * @file NeuralWeights.cpp
 * @brief .nnw file I/O for neural network weight serialization
 */

#include "NeuralWeights.h"

#include <cstdio>
#include <cstring>

namespace Spark::Graphics::Neural
{

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

        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file)
        {
            return false;
        }

        // Write header
        NNWHeader header;
        header.layerCount = static_cast<uint32_t>(network.desc.layers.size());
        header.totalParameters = totalParams;
        std::fwrite(&header, sizeof(NNWHeader), 1, file);

        // Write layer descriptors
        for (const auto& layer : network.desc.layers)
        {
            uint32_t layerData[3] = {layer.inputSize, layer.outputSize, static_cast<uint32_t>(layer.activation)};
            std::fwrite(layerData, sizeof(uint32_t), 3, file);
        }

        // Write weight data
        std::fwrite(network.weights.data(), sizeof(float), totalParams, file);

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

        // Read header
        NNWHeader header;
        if (std::fread(&header, sizeof(NNWHeader), 1, file) != 1)
        {
            std::fclose(file);
            return result;
        }

        if (header.magic != kNNWMagic || header.version != kNNWVersion)
        {
            std::fclose(file);
            return result;
        }

        if (header.layerCount == 0 || header.layerCount > kMaxNetworkLayers)
        {
            std::fclose(file);
            return result;
        }

        // Read layer descriptors
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

        // Validate total parameters match
        if (result.desc.GetTotalParameters() != header.totalParameters)
        {
            std::fclose(file);
            return TrainedNetwork{};
        }

        // Read weight data
        result.weights.resize(header.totalParameters);
        if (std::fread(result.weights.data(), sizeof(float), header.totalParameters, file) != header.totalParameters)
        {
            std::fclose(file);
            return TrainedNetwork{};
        }

        std::fclose(file);
        return result;
    }

} // namespace Spark::Graphics::Neural
