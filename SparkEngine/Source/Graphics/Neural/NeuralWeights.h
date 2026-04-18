/**
 * @file NeuralWeights.h
 * @brief Serialization and deserialization of neural network weights (.nnw format)
 * @author Spark Engine Team
 * @date 2026
 *
 * The .nnw (Neural Network Weights) file format stores a NetworkDesc plus
 * the corresponding float32 weight/bias data in a single binary blob.
 *
 * ## File Layout (v2)
 * ```
 * [NNWHeader]              — magic, version, layer count, total params, flags
 * [LayerDesc] * layerCount — per-layer architecture
 * [float32] * totalParams  — weights + biases, layer by layer
 * [optional] optimizer state (if NNW_FLAG_OPTIMIZER_STATE set):
 *   uint32 optimizerKind   — 0=none 1=Adam 2=SGD-momentum
 *   uint32 stepCount       — optimizer step counter
 *   [float32] * totalParams — first-moment / momentum buffer
 *   [float32] * totalParams — second-moment (Adam only)
 * ```
 *
 * v1 files (no flags, no optimizer state) are still readable — the loader
 * detects version and skips the trailer when flags is zero.
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
    static constexpr uint32_t kNNWVersion = 2;

    /** @brief Feature-flag bit: optimizer-state trailer present. */
    static constexpr uint32_t kNNWFlagOptimizerState = 0x01u;

    /** @brief Optimizer kind enumeration, stored inside the optimizer trailer. */
    enum class OptimizerKind : uint32_t
    {
        None = 0,
        Adam = 1,
        SGDMomentum = 2,
    };

    /**
     * @brief Binary header for .nnw files.
     *
     * v2 adds a @p flags field (previously part of implicit zero padding).
     * Readers must check @p version: v1 files ignore flags entirely.
     */
    struct NNWHeader
    {
        uint32_t magic = kNNWMagic;
        uint32_t version = kNNWVersion;
        uint32_t layerCount = 0;
        uint32_t totalParameters = 0;
        uint32_t flags = 0; ///< Combination of kNNWFlag* bits (v2+)
    };

    /**
     * @brief Optional optimizer-state trailer (serialised when @p kNNWFlagOptimizerState is set).
     *
     * Allows training to resume across runs. Adam needs both moment buffers;
     * SGD-momentum uses only the first. When @p kind == None, the vectors are
     * ignored on save and left empty on load.
     */
    struct OptimizerState
    {
        OptimizerKind kind = OptimizerKind::None;
        uint32_t stepCount = 0;
        std::vector<float> firstMoment;  ///< Adam m / SGD momentum (size == totalParameters)
        std::vector<float> secondMoment; ///< Adam v (size == totalParameters), empty otherwise
    };

    /**
     * @brief Complete in-memory representation of a trained network.
     *
     * Holds the architecture description plus the raw weight data and an
     * optional optimizer state checkpoint.
     */
    struct TrainedNetwork
    {
        NetworkDesc desc;
        std::vector<float> weights; ///< All weights + biases, layer-major order
        OptimizerState optimizer;   ///< Optional — saved only if kind != None
    };

    /**
     * @brief Save a trained network to a .nnw file.
     *
     * Writes v2 format. If @p network.optimizer.kind != None, the trailer
     * is appended; otherwise the file is v2-compatible-with-v1 (flags = 0).
     *
     * @param network The network to save
     * @param path Output file path
     * @return true on success
     */
    bool SaveWeights(const TrainedNetwork& network, const std::string& path);

    /**
     * @brief Load a trained network from a .nnw file.
     *
     * Accepts both v1 and v2 files. v1 files load with @p optimizer.kind = None.
     *
     * @param path Input file path
     * @return Loaded network (empty on failure)
     */
    TrainedNetwork LoadWeights(const std::string& path);

} // namespace Spark::Graphics::Neural
