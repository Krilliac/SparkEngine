/**
 * @file NeuralWeights.h
 * @brief Serialization and deserialization of neural network weights (.nnw format)
 * @author Spark Engine Team
 * @date 2026
 *
 * The .nnw (Neural Network Weights) file format stores a NetworkDesc plus
 * the corresponding float32 weight/bias data in a single binary blob.
 *
 * ## File Layout
 * ```
 * [NNWHeader]              — magic, version, layer count, total params
 * [LayerDesc] * layerCount — per-layer architecture
 * [float32] * totalParams  — weights + biases, layer by layer
 * ```
 */

#pragma once

#include "NeuralTypes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark::Graphics::Neural
{

    /** @brief Magic number for .nnw files ("SNNW" in little-endian). */
    static constexpr uint32_t kNNWMagic = 0x574E4E53; // 'S','N','N','W'

    /** @brief Current .nnw format version. */
    static constexpr uint32_t kNNWVersion = 1;

    /** @brief Binary header for .nnw files. */
    struct NNWHeader
    {
        uint32_t magic = kNNWMagic;
        uint32_t version = kNNWVersion;
        uint32_t layerCount = 0;
        uint32_t totalParameters = 0;
    };

    /**
     * @brief Complete in-memory representation of a trained network.
     *
     * Holds the architecture description plus the raw weight data.
     */
    struct TrainedNetwork
    {
        NetworkDesc desc;
        std::vector<float> weights; ///< All weights + biases, layer-major order
    };

    /**
     * @brief Save a trained network to a .nnw file.
     * @param network The network to save
     * @param path Output file path
     * @return true on success
     */
    bool SaveWeights(const TrainedNetwork& network, const std::string& path);

    /**
     * @brief Load a trained network from a .nnw file.
     * @param path Input file path
     * @return Loaded network (empty on failure)
     */
    TrainedNetwork LoadWeights(const std::string& path);

} // namespace Spark::Graphics::Neural
