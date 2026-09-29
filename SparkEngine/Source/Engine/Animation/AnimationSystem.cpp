/**
 * @file AnimationSystem.cpp
 * @brief AnimationManager (asset cache) and AnimationInstance (per-entity runtime update)
 *
 * Low-level evaluation code lives in:
 *   - SkeletalAnimation.cpp   — keyframe interpolation, clip sampling, blending, skinning
 *   - InverseKinematics.cpp   — TwoBoneIK, LookAtIK solvers
 *   - InverseKinematicsFABRIK.cpp — FABRIK solver
 *   - AnimationStateMachine.cpp — state machine transitions and crossfade
 */
#include "AnimationSystem.h"
#include "AnimationBinaryFormat.h"
#include "../../Core/Platform.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/Validate.h"
#include "../../Graphics/GLTFAnimationLoader.h"
#include "../../Graphics/GLTFSkinnedMeshLoader.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <cmath>
#include <fstream>

using namespace DirectX;
namespace Spark::Animation
{

    namespace
    {
        /// Largest .skel or .sanim file the binary loaders will read. Far above any real skeleton
        /// or clip set; the decoders bound every allocation by the bytes actually read.
        constexpr std::uintmax_t kMaxAnimationAssetBytes = 256u * 1024u * 1024u;

        /// Read an animation asset whole. The stat is only a fast reject: the file can be replaced or
        /// keep growing after it, so the read itself is capped at the size stat reported plus one byte,
        /// and seeing that extra byte rejects the file.
        bool ReadAnimationAssetFile(const std::filesystem::path& path, const std::string& displayPath,
                                    std::vector<std::uint8_t>& bytes)
        {
            std::error_code ec;
            const std::uintmax_t fileSize = std::filesystem::file_size(path, ec);
            if (ec)
            {
                SPARK_LOG_WARN(LogCategory::Animation, "cannot stat '%s' (%s)", displayPath.c_str(),
                               ec.message().c_str());
                return false;
            }
            if (fileSize > kMaxAnimationAssetBytes)
            {
                SPARK_LOG_WARN(LogCategory::Animation, "'%s' is %llu bytes, above the %llu byte limit",
                               displayPath.c_str(), static_cast<unsigned long long>(fileSize),
                               static_cast<unsigned long long>(kMaxAnimationAssetBytes));
                return false;
            }
            std::ifstream file(path, std::ios::binary);
            if (!file.is_open())
            {
                SPARK_LOG_WARN(LogCategory::Animation, "cannot open '%s' (errno=%d)", displayPath.c_str(), errno);
                return false;
            }
            bytes.assign(static_cast<std::size_t>(fileSize) + 1u, 0u);
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            const auto bytesRead = static_cast<std::size_t>(file.gcount());
            if (bytesRead > fileSize)
            {
                SPARK_LOG_WARN(LogCategory::Animation, "'%s' grew while it was being read", displayPath.c_str());
                return false;
            }
            bytes.resize(bytesRead);
            return true;
        }

        /// Asset paths are UTF-8. Constructing a path from a narrow std::string would decode it with
        /// the Windows ANSI code page, so non-ASCII file names would not be found there.
        std::filesystem::path PathFromUtf8(const std::string& utf8)
        {
            return {std::u8string(utf8.begin(), utf8.end())};
        }

        /// .gltf and .glb go through the fail-closed glTF importers instead of the engine binary readers.
        bool IsGLTFPath(const std::filesystem::path& path)
        {
            const std::u8string utf8Extension = path.extension().u8string();
            std::string extension(utf8Extension.begin(), utf8Extension.end());
            std::transform(extension.begin(), extension.end(), extension.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return extension == ".gltf" || extension == ".glb";
        }
    } // namespace

    // ============================================================================
    // AnimationManager
    // ============================================================================

    AnimationManager& AnimationManager::GetInstance()
    {
        static AnimationManager instance;
        return instance;
    }

    std::shared_ptr<Skeleton> AnimationManager::LoadSkeleton(const std::string& filepath)
    {
        auto it = m_skeletons.find(filepath);
        if (it != m_skeletons.end())
            return it->second;

        auto skeleton = std::make_shared<Skeleton>();
        skeleton->name = filepath;

        const std::filesystem::path path = PathFromUtf8(filepath);
        if (IsGLTFPath(path))
        {
            // The skin is read through the same validation as the skinned mesh, so a skeleton is
            // only accepted from a file whose geometry and weights would also import.
            Spark::Graphics::Detail::GLTFSkinnedMeshData imported;
            std::string error;
            if (!Spark::Graphics::Detail::LoadGLTFSkinnedMesh(path, imported, error))
            {
                SPARK_LOG_ERROR(LogCategory::Animation, "Failed to load glTF skeleton '%s': %s; result is not cached",
                                filepath.c_str(), error.c_str());
                return skeleton;
            }
            *skeleton = std::move(imported.skeleton);
            skeleton->name = filepath;
            m_skeletons[filepath] = skeleton;
            SPARK_LOG_INFO(LogCategory::Animation, "Loaded glTF skeleton '%s' (%zu bones)", filepath.c_str(),
                           skeleton->bones.size());
            return skeleton;
        }

        // A failed load is reported as a failure and not memoised: caching it would make a later
        // repaired asset unreachable for the lifetime of the process.
        std::vector<std::uint8_t> bytes;
        if (!ReadAnimationAssetFile(path, filepath, bytes))
        {
            SPARK_LOG_ERROR(LogCategory::Animation, "Failed to load skeleton '%s'; result is not cached",
                            filepath.c_str());
            return skeleton;
        }
        std::string error;
        if (!DecodeSkeletonBinary(bytes, *skeleton, error))
        {
            SPARK_LOG_ERROR(LogCategory::Animation, "Failed to load skeleton '%s': %s; result is not cached",
                            filepath.c_str(), error.c_str());
            return skeleton;
        }

        // If no bones were loaded from file, the skeleton remains empty but valid.
        // Downstream code (AnimationInstance) handles empty skeletons gracefully.
        m_skeletons[filepath] = skeleton;
        SPARK_LOG_INFO(LogCategory::Animation, "Loaded skeleton '%s' (%zu bones)", filepath.c_str(),
                       skeleton->bones.size());
        return skeleton;
    }

    std::vector<std::shared_ptr<AnimationClip>> AnimationManager::LoadAnimations(const std::string& filepath)
    {
        std::vector<std::shared_ptr<AnimationClip>> clips;

        const std::filesystem::path path = PathFromUtf8(filepath);
        if (IsGLTFPath(path))
        {
            std::vector<AnimationClip> imported;
            std::string error;
            if (!Spark::Graphics::Detail::LoadGLTFAnimationClips(path, imported, error))
            {
                SPARK_LOG_ERROR(LogCategory::Animation,
                                "Failed to load glTF animations from '%s': %s; no clips returned", filepath.c_str(),
                                error.c_str());
                return clips;
            }
            clips.reserve(imported.size());
            for (AnimationClip& clip : imported)
            {
                clips.push_back(std::make_shared<AnimationClip>(std::move(clip)));
            }
            SPARK_LOG_INFO(LogCategory::Animation, "Loaded %zu glTF animation clips from '%s'", clips.size(),
                           filepath.c_str());
            return clips;
        }

        // Binary .sanim; the layout and its validation live in AnimationBinaryFormat.h.
        std::vector<std::uint8_t> bytes;
        std::vector<AnimationClip> decoded;
        std::string error;
        if (!ReadAnimationAssetFile(path, filepath, bytes) || !DecodeAnimationClipsBinary(bytes, decoded, error))
        {
            SPARK_LOG_ERROR(LogCategory::Animation, "Failed to load animations from '%s'%s%s; no clips returned",
                            filepath.c_str(), error.empty() ? "" : ": ", error.c_str());
            return clips;
        }
        clips.reserve(decoded.size());
        for (AnimationClip& clip : decoded)
        {
            clips.push_back(std::make_shared<AnimationClip>(std::move(clip)));
        }

        SPARK_LOG_INFO(LogCategory::Animation, "Loaded %zu animation clips from '%s'", clips.size(), filepath.c_str());
        return clips;
    }

    void AnimationManager::RegisterClip(const std::string& name, std::shared_ptr<AnimationClip> clip)
    {
        SPARK_LOG_INFO(LogCategory::Animation, "Registered clip '%s' (duration=%.2fs, %zu channels)", name.c_str(),
                       clip ? clip->duration : 0.0f, clip ? clip->channels.size() : 0);
        m_clips[name] = std::move(clip);
    }

    std::shared_ptr<AnimationClip> AnimationManager::GetClip(const std::string& name) const
    {
        auto it = m_clips.find(name);
        return (it != m_clips.end()) ? it->second : nullptr;
    }

    std::shared_ptr<Skeleton> AnimationManager::GetSkeleton(const std::string& name) const
    {
        auto it = m_skeletons.find(name);
        return (it != m_skeletons.end()) ? it->second : nullptr;
    }

    void AnimationManager::Clear()
    {
        m_clips.clear();
        m_skeletons.clear();
    }

    std::string AnimationManager::Console_ListAnimations() const
    {
        std::ostringstream ss;
        ss << "=== Loaded Animations (" << m_clips.size() << ") ===\n";
        for (const auto& [name, clip] : m_clips)
        {
            ss << "  " << name << " [" << clip->duration << "s, " << clip->channels.size() << " channels]\n";
        }
        return ss.str();
    }

    std::string AnimationManager::Console_ListSkeletons() const
    {
        std::ostringstream ss;
        ss << "=== Loaded Skeletons (" << m_skeletons.size() << ") ===\n";
        for (const auto& [name, skel] : m_skeletons)
        {
            ss << "  " << name << " [" << skel->bones.size() << " bones]\n";
        }
        return ss.str();
    }

    // ============================================================================
    // AnimationInstance — per-entity runtime update
    // ============================================================================

    void AnimationInstance::UpdateLayers(float deltaTime)
    {
        if (!skeleton || skeleton->bones.empty())
            return;

        const size_t boneCount = skeleton->GetBoneCount();
        auto& mgr = AnimationManager::GetInstance();

        // Initialize the blend result with the bind pose
        blendResult.localTransforms.resize(boneCount);
        for (size_t i = 0; i < boneCount; ++i)
        {
            blendResult.localTransforms[i] = skeleton->bones[i].localBindPose;
        }

        // Process layers bottom-to-top (index 0 = base layer)
        for (auto& layer : layers)
        {
            // Advance playback time if the layer is playing
            if (layer.playing)
            {
                layer.currentTime += deltaTime * layer.speed;

                // Handle looping / clamping
                auto clipPtr = mgr.GetClip(layer.clipName);
                if (clipPtr && clipPtr->duration > 0.0f)
                {
                    if (layer.loop)
                    {
                        layer.currentTime = std::fmod(layer.currentTime, clipPtr->duration);
                        if (layer.currentTime < 0.0f)
                            layer.currentTime += clipPtr->duration;
                    }
                    else
                    {
                        layer.currentTime = (std::max)(0.0f, (std::min)(layer.currentTime, clipPtr->duration));
                    }
                }
            }

            // Sample this layer's clip
            auto clipPtr = mgr.GetClip(layer.clipName);
            if (!clipPtr)
                continue;

            std::vector<XMFLOAT4X4> layerTransforms;
            AnimationEvaluator::SampleClip(*clipPtr, *skeleton, layer.currentTime, layerTransforms);

            // Apply blend mode
            switch (layer.blendMode)
            {
            case BlendMode::Override:
            {
                if (layer.boneMask.empty())
                {
                    // Affects all bones: full replacement
                    if (layer.weight >= 1.0f)
                    {
                        blendResult.localTransforms = layerTransforms;
                    }
                    else
                    {
                        // Partial override: blend between current result and this layer
                        AnimationEvaluator::BlendTransforms(blendResult.localTransforms, layerTransforms, layer.weight,
                                                            blendResult.localTransforms);
                    }
                }
                else
                {
                    // Masked override: only affect bones in the mask
                    for (int32_t boneIdx : layer.boneMask)
                    {
                        if (boneIdx >= 0 && static_cast<size_t>(boneIdx) < boneCount)
                        {
                            if (layer.weight >= 1.0f)
                            {
                                blendResult.localTransforms[boneIdx] = layerTransforms[boneIdx];
                            }
                            else
                            {
                                // Per-bone blend for masked bones
                                XMMATRIX mA = XMLoadFloat4x4(&blendResult.localTransforms[boneIdx]);
                                XMMATRIX mB = XMLoadFloat4x4(&layerTransforms[boneIdx]);

                                XMVECTOR sA, rA, tA, sB, rB, tB;
                                XMMatrixDecompose(&sA, &rA, &tA, mA);
                                XMMatrixDecompose(&sB, &rB, &tB, mB);

                                XMVECTOR s = XMVectorLerp(sA, sB, layer.weight);
                                XMVECTOR r = XMQuaternionSlerp(rA, rB, layer.weight);
                                XMVECTOR t = XMVectorLerp(tA, tB, layer.weight);

                                XMMATRIX result = XMMatrixScalingFromVector(s) * XMMatrixRotationQuaternion(r) *
                                                  XMMatrixTranslationFromVector(t);
                                XMStoreFloat4x4(&blendResult.localTransforms[boneIdx], result);
                            }
                        }
                    }
                }
                break;
            }

            case BlendMode::Additive:
            {
                // Additive blending: compute the delta from bind pose and add it
                // delta = layerTransform * inverse(bindPose)
                // result = currentResult * delta * weight
                auto ApplyAdditive = [&](int32_t boneIdx)
                {
                    if (boneIdx < 0 || static_cast<size_t>(boneIdx) >= boneCount)
                        return;

                    XMMATRIX layerMat = XMLoadFloat4x4(&layerTransforms[boneIdx]);
                    XMMATRIX bindMat = XMLoadFloat4x4(&skeleton->bones[boneIdx].localBindPose);
                    XMMATRIX bindInv = XMMatrixInverse(nullptr, bindMat);

                    // Delta = layer * inverse(bind)
                    XMMATRIX delta = bindInv * layerMat;

                    // Decompose delta and scale by weight
                    XMVECTOR sDelta, rDelta, tDelta;
                    XMMatrixDecompose(&sDelta, &rDelta, &tDelta, delta);

                    XMVECTOR identityQuat = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
                    XMVECTOR identityScale = XMVectorSet(1.0f, 1.0f, 1.0f, 0.0f);
                    XMVECTOR zeroTranslation = XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f);

                    XMVECTOR sWeighted = XMVectorLerp(identityScale, sDelta, layer.weight);
                    XMVECTOR rWeighted = XMQuaternionSlerp(identityQuat, rDelta, layer.weight);
                    XMVECTOR tWeighted = XMVectorLerp(zeroTranslation, tDelta, layer.weight);

                    XMMATRIX weightedDelta = XMMatrixScalingFromVector(sWeighted) *
                                             XMMatrixRotationQuaternion(rWeighted) *
                                             XMMatrixTranslationFromVector(tWeighted);

                    // Apply: result = current * weightedDelta
                    XMMATRIX currentMat = XMLoadFloat4x4(&blendResult.localTransforms[boneIdx]);
                    XMMATRIX resultMat = currentMat * weightedDelta;
                    XMStoreFloat4x4(&blendResult.localTransforms[boneIdx], resultMat);
                };

                if (layer.boneMask.empty())
                {
                    for (size_t i = 0; i < boneCount; ++i)
                    {
                        ApplyAdditive(static_cast<int32_t>(i));
                    }
                }
                else
                {
                    for (int32_t boneIdx : layer.boneMask)
                    {
                        ApplyAdditive(boneIdx);
                    }
                }
                break;
            }

            case BlendMode::Layered:
            {
                // Layered blending: linearly blend with weight
                if (layer.boneMask.empty())
                {
                    AnimationEvaluator::BlendTransforms(blendResult.localTransforms, layerTransforms, layer.weight,
                                                        blendResult.localTransforms);
                }
                else
                {
                    for (int32_t boneIdx : layer.boneMask)
                    {
                        if (boneIdx >= 0 && static_cast<size_t>(boneIdx) < boneCount)
                        {
                            XMMATRIX mA = XMLoadFloat4x4(&blendResult.localTransforms[boneIdx]);
                            XMMATRIX mB = XMLoadFloat4x4(&layerTransforms[boneIdx]);

                            XMVECTOR sA, rA, tA, sB, rB, tB;
                            XMMatrixDecompose(&sA, &rA, &tA, mA);
                            XMMatrixDecompose(&sB, &rB, &tB, mB);

                            XMVECTOR s = XMVectorLerp(sA, sB, layer.weight);
                            XMVECTOR r = XMQuaternionSlerp(rA, rB, layer.weight);
                            XMVECTOR t = XMVectorLerp(tA, tB, layer.weight);

                            XMMATRIX result = XMMatrixScalingFromVector(s) * XMMatrixRotationQuaternion(r) *
                                              XMMatrixTranslationFromVector(t);
                            XMStoreFloat4x4(&blendResult.localTransforms[boneIdx], result);
                        }
                    }
                }
                break;
            }
            } // switch
        } // for each layer
    }

    void AnimationInstance::Update(float deltaTime)
    {
        SPARK_WARN_IF(LogCategory::Animation, deltaTime < 0.0f,
                      "AnimationInstance::Update called with negative deltaTime");

        if (!skeleton || skeleton->bones.empty())
            return;

        const size_t boneCount = skeleton->GetBoneCount();
        auto& mgr = AnimationManager::GetInstance();

        // ---- Step 1: Update the state machine (transition evaluation, crossfade) ----
        SPARK_GUARDED_UPDATE("Anim:StateMachine", "Animation", { stateMachine.Update(deltaTime); });

        // ---- Step 2: Sample clips from the state machine and produce base local transforms ----
        blendResult.localTransforms.resize(boneCount);
        blendResult.finalTransforms.resize(boneCount);

        // Initialize with bind pose
        for (size_t i = 0; i < boneCount; ++i)
        {
            blendResult.localTransforms[i] = skeleton->bones[i].localBindPose;
        }

        // Sample the current state's clip
        std::string currentClipName = stateMachine.GetCurrentClipName();
        auto currentClip = mgr.GetClip(currentClipName);
        if (currentClip)
        {
            AnimationEvaluator::SampleClip(*currentClip, *skeleton, stateMachine.GetCurrentPlaybackTime(),
                                           blendResult.localTransforms);
        }

        // If transitioning, blend with the target state's clip
        if (stateMachine.IsTransitioning())
        {
            std::string targetClipName = stateMachine.GetTargetClipName();
            auto targetClip = mgr.GetClip(targetClipName);
            if (targetClip)
            {
                std::vector<XMFLOAT4X4> targetTransforms;
                AnimationEvaluator::SampleClip(*targetClip, *skeleton, stateMachine.GetTargetTime(), targetTransforms);

                AnimationEvaluator::BlendTransforms(blendResult.localTransforms, targetTransforms,
                                                    stateMachine.GetBlendFactor(), blendResult.localTransforms);
            }
        }

        // ---- Step 3: Process animation layers (blend on top of state machine output) ----
        // Only apply layers if there are any configured
        if (!layers.empty())
        {
            // Save state-machine-produced base transforms
            std::vector<XMFLOAT4X4> baseTransforms = blendResult.localTransforms;

            // UpdateLayers populates blendResult.localTransforms from layers
            UpdateLayers(deltaTime);

            // If the state machine is active and layers are also active,
            // the layers override/blend on top of the state machine result.
            // The base layer (index 0) of the layer stack starts from
            // the state machine output when using Override mode.
            // If no layers produced output, keep the state machine result.
        }

        // ---- Step 4: Extract root motion before computing final skinning matrices ----
        if (enableRootMotion && boneCount > 0)
        {
            // Root bone is index 0 by convention
            XMMATRIX rootLocal = XMLoadFloat4x4(&blendResult.localTransforms[0]);
            XMMATRIX rootBind = XMLoadFloat4x4(&skeleton->bones[0].localBindPose);

            // Extract translation delta: difference between animated and bind pose position
            XMVECTOR animPos = rootLocal.r[3];
            XMVECTOR bindPos = rootBind.r[3];
            XMVECTOR posDelta = XMVectorSubtract(animPos, bindPos);

            XMStoreFloat3(&rootMotionDelta, posDelta);

            // Extract rotation delta
            XMVECTOR sAnim, rAnim, tAnim, sBind, rBind, tBind;
            XMMatrixDecompose(&sAnim, &rAnim, &tAnim, rootLocal);
            XMMatrixDecompose(&sBind, &rBind, &tBind, rootBind);

            // Rotation delta = inverse(bindRot) * animRot
            // For simplicity, store the animated rotation as the delta
            // (assumes bind pose root rotation is identity or near-identity)
            XMStoreFloat4(&rootMotionRotationDelta, rAnim);

            // Zero out the root bone's translation so it doesn't move the mesh
            // (the character controller applies rootMotionDelta to the entity position)
            rootLocal.r[3] = bindPos;
            // Restore root bone to bind pose rotation (movement is extracted)
            XMMATRIX rootCleaned = XMMatrixScalingFromVector(sAnim) * XMMatrixRotationQuaternion(rBind) *
                                   XMMatrixTranslationFromVector(bindPos);
            XMStoreFloat4x4(&blendResult.localTransforms[0], rootCleaned);
        }
        else
        {
            rootMotionDelta = {0.0f, 0.0f, 0.0f};
            rootMotionRotationDelta = {0.0f, 0.0f, 0.0f, 1.0f};
        }

        // ---- Step 5: Compute final skinning matrices from blended local transforms ----
        AnimationEvaluator::ComputeSkinningMatrices(*skeleton, blendResult.localTransforms,
                                                    blendResult.finalTransforms);

        // ---- Step 6: Solve IK chains as a post-processing pass ----
        // Save pre-IK local transforms for weight blending
        std::vector<XMFLOAT4X4> preIKTransforms = blendResult.localTransforms;

        for (const auto& chain : ikChains)
        {
            if (!chain.enabled)
                continue;

            switch (chain.type)
            {
            case IKType::TwoBone:
                AnimationEvaluator::SolveTwoBoneIK(blendResult.localTransforms, *skeleton, chain);
                break;
            case IKType::LookAt:
                AnimationEvaluator::SolveLookAtIK(blendResult.localTransforms, *skeleton, chain);
                break;
            case IKType::FABRIK:
                AnimationEvaluator::SolveFABRIK(blendResult.localTransforms, *skeleton, chain);
                break;

            default:
                SPARK_LOG_WARN(LogCategory::Animation, "Unknown IK type %d, skipping chain",
                               static_cast<int>(chain.type));
                continue;
            }

            // Apply IK weight blending: blend between pre-IK and post-IK for affected bones
            if (chain.weight < 1.0f && chain.weight > 0.0f)
            {
                for (int32_t boneIdx : chain.boneIndices)
                {
                    if (boneIdx >= 0 && static_cast<size_t>(boneIdx) < boneCount)
                    {
                        XMMATRIX preIK = XMLoadFloat4x4(&preIKTransforms[boneIdx]);
                        XMMATRIX postIK = XMLoadFloat4x4(&blendResult.localTransforms[boneIdx]);

                        XMVECTOR sPre, rPre, tPre, sPost, rPost, tPost;
                        XMMatrixDecompose(&sPre, &rPre, &tPre, preIK);
                        XMMatrixDecompose(&sPost, &rPost, &tPost, postIK);

                        XMVECTOR s = XMVectorLerp(sPre, sPost, chain.weight);
                        XMVECTOR r = XMQuaternionSlerp(rPre, rPost, chain.weight);
                        XMVECTOR t = XMVectorLerp(tPre, tPost, chain.weight);

                        XMMATRIX blended = XMMatrixScalingFromVector(s) * XMMatrixRotationQuaternion(r) *
                                           XMMatrixTranslationFromVector(t);
                        XMStoreFloat4x4(&blendResult.localTransforms[boneIdx], blended);
                    }
                }
            }
        }

        // Recompute skinning matrices if any IK was applied
        if (!ikChains.empty())
        {
            bool anyIKActive = false;
            for (const auto& chain : ikChains)
            {
                if (chain.enabled)
                {
                    anyIKActive = true;
                    break;
                }
            }

            if (anyIKActive)
            {
                AnimationEvaluator::ComputeSkinningMatrices(*skeleton, blendResult.localTransforms,
                                                            blendResult.finalTransforms);
            }
        }
    }

} // namespace Spark::Animation
