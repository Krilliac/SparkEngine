/**
 * @file GLTFAnimationLoader.h
 * @brief Fail-closed CPU import of glTF 2.0 skin animations into engine AnimationClips.
 */

#pragma once

#include "../Engine/Animation/AnimationTypes.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace Spark::Graphics::Detail
{
    /// Largest accepted keyframe count of one glTF sampler (matches the engine .sanim per-track cap).
    inline constexpr size_t kMaxGLTFAnimationKeys = 1'000'000;

    /**
     * @brief Import every animation of a skinned .gltf/.glb as AnimationClips bound to its skin.
     *
     * The file must first import through LoadGLTFSkinnedMesh (document, skin, geometry, joints and
     * weights), the same importer AnimationManager::LoadSkeleton uses, so clips are only produced
     * when that skeleton loads; clips are produced for that skin's Skeleton, with each channel's boneName and
     * boneIndex set to the skeleton bone of the animated joint. Key times are glTF seconds, so
     * ticksPerSecond is 1 and duration is the latest key time. Clips loop by default because glTF
     * carries no loop flag.
     *
     * A channel replaces a bone's whole local transform when sampled, so a translation, rotation
     * or scale path that the file does not animate is filled with the joint node's rest value.
     * Rotation keys are renormalized.
     *
     * Rejected with a diagnostic: channels that target a non-joint node or the morph "weights"
     * path; STEP and CUBICSPLINE samplers (only LINEAR is supported); animated joints that use a
     * matrix instead of TRS; an animated root joint whose non-joint ancestors are not the identity
     * (the root's bind pose folds those ancestors in); input times that are not finite,
     * non-negative and strictly increasing; output counts that differ from the input count;
     * wrong accessor types; zero-length rotations; duplicate channels for one joint path;
     * animations without channels; duplicate animation names; and more than kMaxGLTFAnimationKeys
     * keys in one sampler.
     *
     * @param path Source .gltf or .glb path.
     * @param clips Replaced with one clip per glTF animation on success (may be empty when the file
     *        has no animations); cleared on failure.
     * @param error Receives a diagnostic on failure.
     * @return true when the file and every animation in it were imported.
     */
    bool LoadGLTFAnimationClips(const std::filesystem::path& path, std::vector<Spark::Animation::AnimationClip>& clips,
                                std::string& error);
} // namespace Spark::Graphics::Detail
