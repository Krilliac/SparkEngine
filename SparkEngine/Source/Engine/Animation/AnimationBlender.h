/**
 * @file AnimationBlender.h
 * @brief Animation blending modes, layers, and blend result structures
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once
#include "../../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif // SPARK_PLATFORM_WINDOWS
#include <string>
#include <vector>
#include <cstdint>


namespace Spark::Animation
{

    /**
 * @brief Specifies how an animation layer's result is combined with lower layers.
 */
    enum class BlendMode
    {
        Override, ///< Fully replace lower layers — no blending, just replacement.
        Additive, ///< Add the delta from bind pose on top of lower layers.
        Layered   ///< Linearly blend with lower layers using the layer's `weight`.
    };

    /**
 * @brief A single animation layer in a multi-layer blending stack.
 *
 * Layers are processed bottom-to-top (index 0 = base). Each layer can affect
 * all bones or only a masked subset, enabling "upper/lower body split" blending.
 *
 * @code
 *   AnimationLayer shootLayer;
 *   shootLayer.clipName  = "Shoot";
 *   shootLayer.weight    = 1.0f;
 *   shootLayer.blendMode = BlendMode::Override;
 *   shootLayer.boneMask  = {spineIdx, armIdx, handIdx};  // upper body only
 * @endcode
 */
    struct AnimationLayer
    {
        /** @brief Name of the AnimationClip this layer plays. Must be registered in AnimationManager. */
        std::string clipName;

        /**
     * @brief Blend weight in [0, 1]. Only used when `blendMode == BlendMode::Layered`.
     *
     * Animate this over time to cross-fade between animations smoothly.
     */
        float weight = 1.0f;

        /** @brief Current playback position within the clip (seconds). Written by the AnimationUpdateSystem. */
        float currentTime = 0.0f;

        /**
     * @brief Playback speed multiplier.
     *
     * 1.0 = normal; >1 = faster; <0 = reverse.
     */
        float speed = 1.0f;

        /** @brief How this layer's output is combined with lower layers. Default: Override. */
        BlendMode blendMode = BlendMode::Override;

        /** @brief Whether this layer is currently advancing its `currentTime`. Default: true. */
        bool playing = true;

        /** @brief Whether the clip loops when it reaches the end. Default: true. */
        bool loop = true;

        /**
     * @brief Optional set of bone indices this layer affects.
     *
     * If empty, ALL bones are affected. If non-empty, only the listed bone indices
     * receive this layer's contribution. Use `Skeleton::FindBone()` to obtain indices.
     */
        std::vector<int32_t> boneMask;
    };

    /**
 * @brief Output of the animation evaluation pass: per-bone transformation matrices.
 *
 * `finalTransforms` is uploaded to the GPU as a constant buffer for skinning in the
 * vertex shader each frame.
 */
    struct BlendResult
    {
        /**
     * @brief Per-bone local transforms in bone-parent space (one matrix per bone).
     *
     * Intermediate result before parent-chain multiplication.
     */
        std::vector<XMFLOAT4X4> localTransforms; ///< Per-bone local transforms

        /**
     * @brief Final per-bone skinning matrices ready for GPU upload (one matrix per bone).
     *
     * Product of parent-chain multiplication and the bone's offset matrix.
     * Upload this to the vertex shader's bone constant buffer.
     */
        std::vector<XMFLOAT4X4> finalTransforms; ///< Per-bone final (skinning) matrices
    };

} // namespace Spark::Animation
