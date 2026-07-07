/**
 * @file AnimationSystem.cpp
 * @brief AnimationManager (asset cache) and AnimationInstance (per-entity runtime update)
 *
 * Low-level evaluation code lives in:
 *   - SkeletalAnimation.cpp   — keyframe interpolation, clip sampling, blending, skinning
 *   - InverseKinematics.cpp   — TwoBoneIK, LookAtIK, FABRIK solvers
 *   - AnimationStateMachine.cpp — state machine transitions and crossfade
 */
#include "AnimationSystem.h"
#include "../../Core/Platform.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/Validate.h"
#include <sstream>
#include <cmath>
#include <fstream>
#include <cstring>

using namespace DirectX;
namespace Spark::Animation
{

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

        // Parse skeleton from Spark Engine binary skeleton format (.skel)
        // Format: [magic:4][version:4][boneCount:4] then per bone:
        //   [nameLen:4][name:nameLen][parentIndex:4][offsetMatrix:64][localBindPose:64]
        std::ifstream file(filepath, std::ios::binary);
        if (file.is_open())
        {
            char magic[4] = {};
            file.read(magic, 4);

            if (std::memcmp(magic, "SKEL", 4) == 0)
            {
                uint32_t version = 0;
                file.read(reinterpret_cast<char*>(&version), sizeof(version));
                if (!file.good())
                {
                    SPARK_LOG_WARN(LogCategory::Animation, "Skeleton file '%s' truncated before version field",
                                   filepath.c_str());
                    m_skeletons[filepath] = skeleton;
                    return skeleton;
                }

                uint32_t boneCount = 0;
                file.read(reinterpret_cast<char*>(&boneCount), sizeof(boneCount));

                // Bound the count read from an untrusted file before reserving — a corrupt
                // .skel with boneCount near 0xFFFFFFFF would otherwise trigger a multi-GB
                // allocation (bad_alloc / DoS) before the file.good()-bounded loop runs.
                constexpr uint32_t kMaxBones = 100'000;
                if (!file.good() || boneCount > kMaxBones)
                {
                    SPARK_LOG_WARN(LogCategory::Animation, "Skeleton file '%s' has invalid bone count %u (max %u)",
                                   filepath.c_str(), boneCount, kMaxBones);
                    m_skeletons[filepath] = skeleton;
                    return skeleton;
                }

                skeleton->bones.reserve(boneCount);

                for (uint32_t i = 0; i < boneCount && file.good(); ++i)
                {
                    Bone bone;

                    // Read bone name (length-prefixed string)
                    uint32_t nameLen = 0;
                    file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
                    if (nameLen > 0 && nameLen < 256)
                    {
                        bone.name.resize(nameLen);
                        file.read(bone.name.data(), nameLen);
                    }

                    // Read parent index
                    file.read(reinterpret_cast<char*>(&bone.parentIndex), sizeof(bone.parentIndex));

                    // Read offset matrix (inverse bind pose) - 16 floats, row-major
                    file.read(reinterpret_cast<char*>(&bone.offsetMatrix), sizeof(XMFLOAT4X4));

                    // Read local bind pose - 16 floats, row-major
                    file.read(reinterpret_cast<char*>(&bone.localBindPose), sizeof(XMFLOAT4X4));

                    skeleton->boneNameToIndex[bone.name] = static_cast<int32_t>(i);
                    skeleton->bones.push_back(std::move(bone));
                }
            }
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

        // Parse animation clips from Spark Engine binary animation format (.sanim)
        // Format: [magic:4][version:4][clipCount:4] then per clip:
        //   [nameLen:4][name:nameLen][duration:4][ticksPerSecond:4][loop:1]
        //   [channelCount:4] then per channel:
        //     [boneNameLen:4][boneName:boneNameLen][boneIndex:4]
        //     [posKeyCount:4] then per key: [time:4][x:4][y:4][z:4]
        //     [rotKeyCount:4] then per key: [time:4][x:4][y:4][z:4][w:4]
        //     [sclKeyCount:4] then per key: [time:4][x:4][y:4][z:4]
        std::ifstream file(filepath, std::ios::binary);
        if (!file.is_open())
        {
            SPARK_LOG_WARN(LogCategory::Animation, "LoadAnimations: cannot open '%s' (errno=%d)", filepath.c_str(),
                           errno);
            return clips;
        }

        char magic[4] = {};
        file.read(magic, 4);

        if (std::memcmp(magic, "ANIM", 4) != 0)
        {
            SPARK_LOG_WARN(LogCategory::Animation, "LoadAnimations: '%s' is not a .sanim file (bad magic '%c%c%c%c')",
                           filepath.c_str(), magic[0], magic[1], magic[2], magic[3]);
            return clips;
        }

        uint32_t version = 0;
        file.read(reinterpret_cast<char*>(&version), sizeof(version));

        uint32_t clipCount = 0;
        file.read(reinterpret_cast<char*>(&clipCount), sizeof(clipCount));
        // Bound the count from an untrusted file before reserving (matches the per-channel
        // and per-keyframe caps below) so a corrupt clipCount cannot force a huge alloc.
        constexpr uint32_t kMaxClips = 100'000;
        if (!file.good() || clipCount > kMaxClips)
        {
            SPARK_LOG_WARN(LogCategory::Animation, "LoadAnimations: invalid clip count %u in '%s'", clipCount,
                           filepath.c_str());
            return clips;
        }

        clips.reserve(clipCount);

        for (uint32_t c = 0; c < clipCount && file.good(); ++c)
        {
            auto clip = std::make_shared<AnimationClip>();

            // Read clip name
            uint32_t nameLen = 0;
            file.read(reinterpret_cast<char*>(&nameLen), sizeof(nameLen));
            if (nameLen > 0 && nameLen < 256)
            {
                clip->name.resize(nameLen);
                file.read(clip->name.data(), nameLen);
            }

            // Read clip metadata
            file.read(reinterpret_cast<char*>(&clip->duration), sizeof(float));
            file.read(reinterpret_cast<char*>(&clip->ticksPerSecond), sizeof(float));

            uint8_t loopByte = 0;
            file.read(reinterpret_cast<char*>(&loopByte), sizeof(loopByte));
            clip->loop = (loopByte != 0);

            // Read channels
            uint32_t channelCount = 0;
            file.read(reinterpret_cast<char*>(&channelCount), sizeof(channelCount));
            // Sanity cap: prevent malformed files from causing huge allocations
            constexpr uint32_t kMaxChannels = 10'000;
            if (!file.good() || channelCount > kMaxChannels)
            {
                SPARK_LOG_WARN(LogCategory::Animation, "Invalid channel count %u in '%s'", channelCount,
                               filepath.c_str());
                break;
            }
            clip->channels.reserve(channelCount);

            for (uint32_t ch = 0; ch < channelCount && file.good(); ++ch)
            {
                BoneAnimation boneAnim;

                // Read bone name for this channel
                uint32_t boneNameLen = 0;
                file.read(reinterpret_cast<char*>(&boneNameLen), sizeof(boneNameLen));
                if (boneNameLen > 0 && boneNameLen < 256)
                {
                    boneAnim.boneName.resize(boneNameLen);
                    file.read(boneAnim.boneName.data(), boneNameLen);
                }

                // Read cached bone index (-1 if unresolved)
                file.read(reinterpret_cast<char*>(&boneAnim.boneIndex), sizeof(boneAnim.boneIndex));

                // Read position keyframes
                uint32_t posKeyCount = 0;
                file.read(reinterpret_cast<char*>(&posKeyCount), sizeof(posKeyCount));
                constexpr uint32_t kMaxKeyframes = 1'000'000;
                if (posKeyCount > kMaxKeyframes)
                    break;
                boneAnim.positionKeys.resize(posKeyCount);
                for (uint32_t k = 0; k < posKeyCount && file.good(); ++k)
                {
                    file.read(reinterpret_cast<char*>(&boneAnim.positionKeys[k].time), sizeof(float));
                    file.read(reinterpret_cast<char*>(&boneAnim.positionKeys[k].value), sizeof(XMFLOAT3));
                }

                // Read rotation keyframes
                uint32_t rotKeyCount = 0;
                file.read(reinterpret_cast<char*>(&rotKeyCount), sizeof(rotKeyCount));
                if (rotKeyCount > kMaxKeyframes)
                    break;
                boneAnim.rotationKeys.resize(rotKeyCount);
                for (uint32_t k = 0; k < rotKeyCount && file.good(); ++k)
                {
                    file.read(reinterpret_cast<char*>(&boneAnim.rotationKeys[k].time), sizeof(float));
                    file.read(reinterpret_cast<char*>(&boneAnim.rotationKeys[k].value), sizeof(XMFLOAT4));
                }

                // Read scale keyframes
                uint32_t sclKeyCount = 0;
                file.read(reinterpret_cast<char*>(&sclKeyCount), sizeof(sclKeyCount));
                if (sclKeyCount > kMaxKeyframes)
                    break;
                boneAnim.scaleKeys.resize(sclKeyCount);
                for (uint32_t k = 0; k < sclKeyCount && file.good(); ++k)
                {
                    file.read(reinterpret_cast<char*>(&boneAnim.scaleKeys[k].time), sizeof(float));
                    file.read(reinterpret_cast<char*>(&boneAnim.scaleKeys[k].value), sizeof(XMFLOAT3));
                }

                clip->channels.push_back(std::move(boneAnim));
            }

            clips.push_back(std::move(clip));
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
