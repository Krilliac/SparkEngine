/**
 * @file AnimationBinaryFormat.h
 * @brief Decoders for the engine's binary skeleton (.skel) and animation clip (.sanim) files.
 *
 * AnimationManager::LoadSkeleton and AnimationManager::LoadAnimations read a whole file with a
 * bounded read and hand its bytes to these decoders. Skeletons and clips ship with mods and
 * asset packs, so every byte is untrusted.
 *
 * Contract:
 * - Thread affinity: thread-agnostic. The decoders hold no global or static mutable state and
 *   touch only their arguments.
 * - Ownership: the caller owns the input span and the output objects. Output is written only
 *   when the whole input decodes and validates; on failure it is left exactly as passed in and
 *   `error` names the first problem.
 * - Allocation: bounded by the input size. Every declared count is checked against the bytes
 *   that remain (at the smallest encoding of one record) before anything is reserved or resized,
 *   on top of the fixed per-file caps below.
 * - Scalability: linear in the input size; one pass, no seeking.
 *
 * Layout (little-endian, as written by the engine tooling):
 * - .skel: [magic "SKEL"][version:4][boneCount:4] then per bone
 *   [nameLen:4][name][parentIndex:4][offsetMatrix:64][localBindPose:64]
 * - .sanim: [magic "ANIM"][version:4][clipCount:4] then per clip
 *   [nameLen:4][name][duration:4][ticksPerSecond:4][loop:1][channelCount:4] then per channel
 *   [boneNameLen:4][boneName][boneIndex:4][posKeyCount:4][pos keys: time + xyz]
 *   [rotKeyCount:4][rot keys: time + xyzw][sclKeyCount:4][scale keys: time + xyz]
 *
 * Validation beyond the layout: version is kAnimationBinaryVersion; names are shorter than
 * kMaxAnimationAssetNameLength; a parent precedes its child; matrices, clip timing (finite and
 * non-negative) and key values are finite; key times are finite and non-decreasing per track.
 */

#pragma once

#include "AnimationTypes.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace Spark::Animation
{
    /// The only on-disk version of both formats.
    constexpr std::uint32_t kAnimationBinaryVersion = 1;

    /// A name length prefix at or above this is malformed (the name is not skipped: the reader rejects the file).
    constexpr std::uint32_t kMaxAnimationAssetNameLength = 256;

    constexpr std::uint32_t kMaxSkeletonBones = 100'000;
    constexpr std::uint32_t kMaxAnimationClips = 100'000;
    constexpr std::uint32_t kMaxAnimationChannels = 10'000;
    constexpr std::uint32_t kMaxAnimationKeyframes = 1'000'000;

    /**
     * @brief Decode a .skel file.
     * @param bytes The whole file.
     * @param out Receives the bones and name index on success; its name is left to the caller.
     * @param error Receives the reason on failure.
     * @return true when the file decoded and validated.
     */
    bool DecodeSkeletonBinary(std::span<const std::uint8_t> bytes, Skeleton& out, std::string& error);

    /**
     * @brief Decode a .sanim file.
     * @param bytes The whole file.
     * @param out Replaced by the decoded clips on success.
     * @param error Receives the reason on failure.
     * @return true when the file decoded and validated.
     */
    bool DecodeAnimationClipsBinary(std::span<const std::uint8_t> bytes, std::vector<AnimationClip>& out,
                                    std::string& error);

} // namespace Spark::Animation
