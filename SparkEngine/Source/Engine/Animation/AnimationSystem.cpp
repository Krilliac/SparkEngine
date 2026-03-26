/**
 * @file AnimationSystem.cpp
 * @brief Implementation of skeletal animation system
 */
#include "../../Core/Platform.h"
#include "AnimationSystem.h"
#include "../../Utils/Validate.h"
#include <sstream>
#include <cmath>
#include <fstream>
#include <cstring>

using namespace DirectX;
namespace Spark::Animation
{

    // ============================================================================
    // BoneAnimation — Keyframe Interpolation
    // ============================================================================

    template <typename T> static int FindKeyIndex(const std::vector<T>& keys, float time)
    {
        // Binary search for sorted keyframes — O(log n) instead of O(n)
        if (keys.size() <= 2)
        {
            // Fast path for trivial cases (common: 1 or 2 keyframes)
            if (keys.size() <= 1 || time < keys[1].time)
                return 0;
            return static_cast<int>(keys.size()) - 1;
        }

        int lo = 0;
        int hi = static_cast<int>(keys.size()) - 1;
        while (lo < hi - 1)
        {
            int mid = lo + (hi - lo) / 2;
            if (time < keys[mid].time)
                hi = mid;
            else
                lo = mid;
        }
        return lo;
    }

    XMFLOAT3 BoneAnimation::InterpolatePosition(float time) const
    {
        if (positionKeys.empty())
            return {0, 0, 0};
        if (positionKeys.size() == 1)
            return positionKeys[0].value;

        int idx = FindKeyIndex(positionKeys, time);
        if (idx >= static_cast<int>(positionKeys.size()) - 1)
            return positionKeys.back().value;

        const auto& k0 = positionKeys[idx];
        const auto& k1 = positionKeys[idx + 1];
        float dt = k1.time - k0.time;
        float t = (dt > 0.0f) ? (time - k0.time) / dt : 0.0f;
        t = (std::max)(0.0f, (std::min)(1.0f, t));

        XMVECTOR v0 = XMLoadFloat3(&k0.value);
        XMVECTOR v1 = XMLoadFloat3(&k1.value);
        XMFLOAT3 result;
        XMStoreFloat3(&result, XMVectorLerp(v0, v1, t));
        return result;
    }

    XMFLOAT4 BoneAnimation::InterpolateRotation(float time) const
    {
        if (rotationKeys.empty())
            return {0, 0, 0, 1};
        if (rotationKeys.size() == 1)
            return rotationKeys[0].value;

        int idx = FindKeyIndex(rotationKeys, time);
        if (idx >= static_cast<int>(rotationKeys.size()) - 1)
            return rotationKeys.back().value;

        const auto& k0 = rotationKeys[idx];
        const auto& k1 = rotationKeys[idx + 1];
        float dt = k1.time - k0.time;
        float t = (dt > 0.0f) ? (time - k0.time) / dt : 0.0f;
        t = (std::max)(0.0f, (std::min)(1.0f, t));

        XMVECTOR q0 = XMLoadFloat4(&k0.value);
        XMVECTOR q1 = XMLoadFloat4(&k1.value);
        XMFLOAT4 result;
        XMStoreFloat4(&result, XMQuaternionSlerp(q0, q1, t));
        return result;
    }

    XMFLOAT3 BoneAnimation::InterpolateScale(float time) const
    {
        if (scaleKeys.empty())
            return {1, 1, 1};
        if (scaleKeys.size() == 1)
            return scaleKeys[0].value;

        int idx = FindKeyIndex(scaleKeys, time);
        if (idx >= static_cast<int>(scaleKeys.size()) - 1)
            return scaleKeys.back().value;

        const auto& k0 = scaleKeys[idx];
        const auto& k1 = scaleKeys[idx + 1];
        float dt = k1.time - k0.time;
        float t = (dt > 0.0f) ? (time - k0.time) / dt : 0.0f;
        t = (std::max)(0.0f, (std::min)(1.0f, t));

        XMVECTOR v0 = XMLoadFloat3(&k0.value);
        XMVECTOR v1 = XMLoadFloat3(&k1.value);
        XMFLOAT3 result;
        XMStoreFloat3(&result, XMVectorLerp(v0, v1, t));
        return result;
    }

    // ============================================================================
    // AnimationStateMachine
    // ============================================================================

    void AnimationStateMachine::AddState(const AnimationState& state)
    {
        m_states[state.name] = state;
        SPARK_LOG_DEBUG(LogCategory::Animation, "State machine: added state '%s' (clip='%s', speed=%.2f)",
                        state.name.c_str(), state.clipName.c_str(), state.speed);
        if (m_defaultState.empty())
            m_defaultState = state.name;
        if (m_currentState.empty())
            m_currentState = state.name;
    }

    void AnimationStateMachine::AddTransition(const AnimationTransition& transition)
    {
        m_transitions.push_back(transition);
    }

    void AnimationStateMachine::SetDefaultState(const std::string& stateName)
    {
        m_defaultState = stateName;
        if (m_currentState.empty())
            m_currentState = stateName;
    }

    void AnimationStateMachine::Update(float deltaTime)
    {
        SPARK_WARN_IF(LogCategory::Animation, deltaTime < 0.0f,
                      "AnimationStateMachine::Update called with negative deltaTime");

        if (m_currentState.empty())
        {
            if (!m_defaultState.empty())
            {
                m_currentState = m_defaultState;
            }
            else
            {
                return;
            }
        }

        // Advance current state playback time
        auto currentIt = m_states.find(m_currentState);
        if (currentIt != m_states.end())
        {
            m_currentTime += deltaTime * currentIt->second.speed;
        }

        if (m_isTransitioning)
        {
            // Advance the target state's playback time during the crossfade
            auto targetIt = m_states.find(m_targetState);
            if (targetIt != m_states.end())
            {
                m_targetTime += deltaTime * targetIt->second.speed;
            }

            m_transitionElapsed += deltaTime;
            m_blendFactor =
                (m_transitionDuration > 0.0f) ? (std::min)(m_transitionElapsed / m_transitionDuration, 1.0f) : 1.0f;

            if (m_blendFactor >= 1.0f)
            {
                // Transition complete: target becomes current
                SPARK_LOG_INFO(LogCategory::Animation, "State machine transition complete: now in '%s'",
                               m_targetState.c_str());
                m_currentState = m_targetState;
                m_currentTime = m_targetTime;
                m_targetState.clear();
                m_targetTime = 0.0f;
                m_isTransitioning = false;
                m_blendFactor = 0.0f;
                m_transitionElapsed = 0.0f;
                m_transitionDuration = 0.0f;
            }
            return;
        }

        // Evaluate transitions from the current state
        // Compute normalized time for exit-time checks
        float normalizedTime = 0.0f;
        if (currentIt != m_states.end())
        {
            auto clipPtr = AnimationManager::GetInstance().GetClip(currentIt->second.clipName);
            if (clipPtr && clipPtr->duration > 0.0f)
            {
                normalizedTime = m_currentTime / clipPtr->duration;
            }
        }

        for (const auto& t : m_transitions)
        {
            bool fromMatch = (t.fromState == m_currentState) || (t.fromState == "*");
            if (!fromMatch)
                continue;

            // Check exit time requirement
            if (t.hasExitTime && normalizedTime < t.exitTime)
                continue;

            // Check condition
            if (t.condition && !t.condition())
                continue;

            // Fire the transition
            SPARK_LOG_INFO(LogCategory::Animation, "State machine transition: '%s' -> '%s' (duration=%.2fs)",
                           m_currentState.c_str(), t.toState.c_str(), t.duration);
            m_targetState = t.toState;
            m_transitionDuration = t.duration;
            m_transitionElapsed = 0.0f;
            m_targetTime = 0.0f;
            m_isTransitioning = true;
            m_blendFactor = 0.0f;
            break;
        }
    }

    std::string AnimationStateMachine::GetCurrentClipName() const
    {
        auto it = m_states.find(m_currentState);
        if (it != m_states.end())
        {
            return it->second.clipName;
        }
        return {};
    }

    std::string AnimationStateMachine::GetTargetClipName() const
    {
        if (!m_isTransitioning)
            return {};
        auto it = m_states.find(m_targetState);
        if (it != m_states.end())
        {
            return it->second.clipName;
        }
        return {};
    }

    void AnimationStateMachine::ForceState(const std::string& stateName)
    {
        if (m_states.contains(stateName))
        {
            SPARK_LOG_INFO(LogCategory::Animation, "State machine: forced state '%s' (was '%s')", stateName.c_str(),
                           m_currentState.c_str());
            m_currentState = stateName;
            m_isTransitioning = false;
            m_currentTime = 0.0f;
            m_blendFactor = 0.0f;
        }
        else
        {
            SPARK_LOG_WARN(LogCategory::Animation, "State machine: ForceState('%s') failed - state not found",
                           stateName.c_str());
        }
    }

    std::string AnimationStateMachine::Console_GetStateInfo() const
    {
        std::ostringstream ss;
        ss << "Current State: " << m_currentState << "\n";
        ss << "Time: " << m_currentTime << "s\n";
        if (m_isTransitioning)
        {
            ss << "Transitioning to: " << m_targetState << "\n";
            ss << "Blend: " << (m_blendFactor * 100.0f) << "%\n";
        }
        ss << "States (" << m_states.size() << "): ";
        for (const auto& [name, _] : m_states)
            ss << name << " ";
        ss << "\n";
        return ss.str();
    }

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

                uint32_t boneCount = 0;
                file.read(reinterpret_cast<char*>(&boneCount), sizeof(boneCount));

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
            return clips;

        char magic[4] = {};
        file.read(magic, 4);

        if (std::memcmp(magic, "ANIM", 4) != 0)
            return clips;

        uint32_t version = 0;
        file.read(reinterpret_cast<char*>(&version), sizeof(version));

        uint32_t clipCount = 0;
        file.read(reinterpret_cast<char*>(&clipCount), sizeof(clipCount));

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
    // AnimationEvaluator
    // ============================================================================

    void AnimationEvaluator::SampleClip(const AnimationClip& clip, const Skeleton& skeleton, float time,
                                        std::vector<XMFLOAT4X4>& outLocalTransforms)
    {
        size_t boneCount = skeleton.bones.size();
        outLocalTransforms.resize(boneCount);

        // Initialize with bind pose
        for (size_t i = 0; i < boneCount; ++i)
            outLocalTransforms[i] = skeleton.bones[i].localBindPose;

        // Wrap time for looping
        float animTime = time;
        if (clip.duration > 0.0f && clip.loop)
        {
            animTime = std::fmod(time, clip.duration);
            if (animTime < 0.0f)
                animTime += clip.duration;
        }

        // Sample each channel
        for (const auto& channel : clip.channels)
        {
            int32_t boneIdx = channel.boneIndex;
            if (boneIdx < 0)
            {
                boneIdx = skeleton.FindBone(channel.boneName);
                if (boneIdx < 0)
                    continue;
            }

            XMFLOAT3 pos = channel.InterpolatePosition(animTime);
            XMFLOAT4 rot = channel.InterpolateRotation(animTime);
            XMFLOAT3 scl = channel.InterpolateScale(animTime);

            XMMATRIX S = XMMatrixScaling(scl.x, scl.y, scl.z);
            XMMATRIX R = XMMatrixRotationQuaternion(XMLoadFloat4(&rot));
            XMMATRIX T = XMMatrixTranslation(pos.x, pos.y, pos.z);
            XMMATRIX local = S * R * T;

            XMStoreFloat4x4(&outLocalTransforms[boneIdx], local);
        }
    }

    void AnimationEvaluator::BlendTransforms(const std::vector<XMFLOAT4X4>& a, const std::vector<XMFLOAT4X4>& b,
                                             float blendFactor, std::vector<XMFLOAT4X4>& outResult)
    {
        size_t count = (std::min)(a.size(), b.size());
        outResult.resize(count);

        for (size_t i = 0; i < count; ++i)
        {
            XMMATRIX mA = XMLoadFloat4x4(&a[i]);
            XMMATRIX mB = XMLoadFloat4x4(&b[i]);

            // Decompose, interpolate, recompose
            XMVECTOR sA, rA, tA, sB, rB, tB;
            XMMatrixDecompose(&sA, &rA, &tA, mA);
            XMMatrixDecompose(&sB, &rB, &tB, mB);

            XMVECTOR s = XMVectorLerp(sA, sB, blendFactor);
            XMVECTOR r = XMQuaternionSlerp(rA, rB, blendFactor);
            XMVECTOR t = XMVectorLerp(tA, tB, blendFactor);

            XMMATRIX result =
                XMMatrixScalingFromVector(s) * XMMatrixRotationQuaternion(r) * XMMatrixTranslationFromVector(t);
            XMStoreFloat4x4(&outResult[i], result);
        }
    }

    void AnimationEvaluator::ComputeSkinningMatrices(const Skeleton& skeleton,
                                                     const std::vector<XMFLOAT4X4>& localTransforms,
                                                     std::vector<XMFLOAT4X4>& outFinalTransforms)
    {
        size_t boneCount = skeleton.bones.size();
        outFinalTransforms.resize(boneCount);

        // Compute global transforms by traversing hierarchy
        std::vector<XMMATRIX> globalTransforms(boneCount);

        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;

            if (parentIdx >= 0 && parentIdx < static_cast<int32_t>(boneCount))
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Multiply by offset (inverse bind pose) to get final skinning matrices
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX offset = XMLoadFloat4x4(&skeleton.bones[i].offsetMatrix);
            XMMATRIX finalMat = offset * globalTransforms[i];
            XMStoreFloat4x4(&outFinalTransforms[i], finalMat);
        }
    }

    void AnimationEvaluator::SolveTwoBoneIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                            const IKChain& chain)
    {
        if (!chain.enabled || chain.boneIndices.size() < 3)
            return;

        const int32_t rootIdx = chain.boneIndices[0];
        const int32_t midIdx = chain.boneIndices[1];
        const int32_t endIdx = chain.boneIndices[2];
        const size_t boneCount = skeleton.bones.size();

        if (rootIdx < 0 || midIdx < 0 || endIdx < 0)
            return;
        if (static_cast<size_t>(rootIdx) >= boneCount || static_cast<size_t>(midIdx) >= boneCount ||
            static_cast<size_t>(endIdx) >= boneCount)
            return;

        // Step 1: Compute world (global) transforms for all bones so we can
        // extract the three joint world positions accurately.
        std::vector<XMMATRIX> globalTransforms(boneCount);
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;
            if (parentIdx >= 0 && static_cast<size_t>(parentIdx) < boneCount)
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Extract world-space positions of root, mid (elbow/knee), and end (hand/foot)
        XMVECTOR rootPos = globalTransforms[rootIdx].r[3];
        XMVECTOR midPos = globalTransforms[midIdx].r[3];
        XMVECTOR endPos = globalTransforms[endIdx].r[3];
        XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
        XMVECTOR poleVec = XMLoadFloat3(&chain.poleVector);

        // Compute upper and lower bone lengths from the current pose
        float upperLen = XMVectorGetX(XMVector3Length(XMVectorSubtract(midPos, rootPos)));
        float lowerLen = XMVectorGetX(XMVector3Length(XMVectorSubtract(endPos, midPos)));

        if (upperLen < 1e-6f || lowerLen < 1e-6f)
            return;

        XMVECTOR rootToTarget = XMVectorSubtract(target, rootPos);
        float targetDist = XMVectorGetX(XMVector3Length(rootToTarget));

        if (targetDist < 1e-6f)
            return;

        // Clamp target distance to the reachable range so the cosine law stays valid
        float maxReach = upperLen + lowerLen - 1e-4f;
        float minReach = std::fabs(upperLen - lowerLen) + 1e-4f;
        float clampedDist = (std::max)(minReach, (std::min)(maxReach, targetDist));

        // Step 2: Use the cosine law to find the angle at root and mid joints.
        // cos(angleAtRoot) = (upper^2 + dist^2 - lower^2) / (2 * upper * dist)
        float cosAngleRoot =
            (upperLen * upperLen + clampedDist * clampedDist - lowerLen * lowerLen) / (2.0f * upperLen * clampedDist);
        cosAngleRoot = (std::max)(-1.0f, (std::min)(1.0f, cosAngleRoot));
        float angleAtRoot = std::acos(cosAngleRoot);

        // Step 3: Compute the IK plane using the pole vector.
        // The chain lies in the plane defined by (rootToTarget direction, pole hint).
        XMVECTOR targetDir = XMVector3Normalize(rootToTarget);

        // Project pole vector onto the plane perpendicular to targetDir
        XMVECTOR poleDir = XMVectorSubtract(poleVec, rootPos);
        float poleDot = XMVectorGetX(XMVector3Dot(poleDir, targetDir));
        XMVECTOR poleOnPlane = XMVectorSubtract(poleDir, XMVectorScale(targetDir, poleDot));
        float poleOnPlaneLen = XMVectorGetX(XMVector3Length(poleOnPlane));

        XMVECTOR bendAxis;
        if (poleOnPlaneLen > 1e-6f)
        {
            bendAxis = XMVector3Normalize(poleOnPlane);
        }
        else
        {
            // Fallback: use an arbitrary perpendicular vector
            XMVECTOR up = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            bendAxis = XMVector3Cross(targetDir, up);
            if (XMVectorGetX(XMVector3LengthSq(bendAxis)) < 1e-6f)
            {
                up = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
                bendAxis = XMVector3Cross(targetDir, up);
            }
            bendAxis = XMVector3Normalize(bendAxis);
        }

        // The normal of the IK plane
        XMVECTOR ikPlaneNormal = XMVector3Normalize(XMVector3Cross(targetDir, bendAxis));

        // Step 4: Compute the desired mid-joint (elbow/knee) world position.
        // Rotate targetDir by angleAtRoot around ikPlaneNormal to find the upper bone direction.
        XMMATRIX rootRot = XMMatrixRotationAxis(ikPlaneNormal, angleAtRoot);
        XMVECTOR upperDir = XMVector3Normalize(XMVector3TransformCoord(targetDir, rootRot));
        XMVECTOR desiredMidPos = XMVectorAdd(rootPos, XMVectorScale(upperDir, upperLen));

        // Step 5: Apply the rotation to the root bone.
        // Compute the rotation from the current root->mid direction to the desired direction.
        XMVECTOR currentRootToMid = XMVector3Normalize(XMVectorSubtract(midPos, rootPos));
        XMVECTOR desiredRootToMid = XMVector3Normalize(XMVectorSubtract(desiredMidPos, rootPos));

        // Build a delta rotation quaternion from currentRootToMid to desiredRootToMid
        XMVECTOR rootRotAxis = XMVector3Cross(currentRootToMid, desiredRootToMid);
        float rootRotDot = XMVectorGetX(XMVector3Dot(currentRootToMid, desiredRootToMid));
        rootRotDot = (std::max)(-1.0f, (std::min)(1.0f, rootRotDot));

        if (XMVectorGetX(XMVector3LengthSq(rootRotAxis)) > 1e-10f)
        {
            float rootRotAngle = std::acos(rootRotDot);
            XMMATRIX rootDeltaRot = XMMatrixRotationAxis(XMVector3Normalize(rootRotAxis), rootRotAngle);

            // Convert world-space rotation to local-space:
            // newLocal = localTransform * inverse(parentWorld) * deltaRotWorld * parentWorld
            // Simplified: apply delta rotation in world space then convert back
            XMMATRIX rootLocal = XMLoadFloat4x4(&localTransforms[rootIdx]);
            int32_t rootParent = skeleton.bones[rootIdx].parentIndex;
            if (rootParent >= 0 && static_cast<size_t>(rootParent) < boneCount)
            {
                XMMATRIX parentWorldInv = XMMatrixInverse(nullptr, globalTransforms[rootParent]);
                XMMATRIX newGlobal = rootDeltaRot * globalTransforms[rootIdx];
                XMMATRIX newLocal = newGlobal * parentWorldInv;
                // Preserve the local translation (bone position doesn't change, only rotation)
                newLocal.r[3] = rootLocal.r[3];
                XMStoreFloat4x4(&localTransforms[rootIdx], newLocal);
            }
            else
            {
                XMMATRIX newLocal = rootDeltaRot * rootLocal;
                newLocal.r[3] = rootLocal.r[3];
                XMStoreFloat4x4(&localTransforms[rootIdx], newLocal);
            }
        }

        // Step 6: Recompute global transforms after root bone modification
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;
            if (parentIdx >= 0 && static_cast<size_t>(parentIdx) < boneCount)
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Step 7: Apply the rotation to the mid bone (elbow/knee bend).
        // Compute the desired end-effector position from the new mid position.
        XMVECTOR newMidPos = globalTransforms[midIdx].r[3];
        XMVECTOR currentMidToEnd = XMVector3Normalize(XMVectorSubtract(endPos, newMidPos));

        // The desired direction from mid to end effector: toward target from the new mid position
        XMVECTOR desiredEndPos = target;
        XMVECTOR desiredMidToEnd = XMVector3Normalize(XMVectorSubtract(desiredEndPos, newMidPos));

        XMVECTOR midRotAxis = XMVector3Cross(currentMidToEnd, desiredMidToEnd);
        float midRotDot = XMVectorGetX(XMVector3Dot(currentMidToEnd, desiredMidToEnd));
        midRotDot = (std::max)(-1.0f, (std::min)(1.0f, midRotDot));

        if (XMVectorGetX(XMVector3LengthSq(midRotAxis)) > 1e-10f)
        {
            float midRotAngle = std::acos(midRotDot);
            XMMATRIX midDeltaRot = XMMatrixRotationAxis(XMVector3Normalize(midRotAxis), midRotAngle);

            XMMATRIX midLocal = XMLoadFloat4x4(&localTransforms[midIdx]);
            int32_t midParent = skeleton.bones[midIdx].parentIndex;
            if (midParent >= 0 && static_cast<size_t>(midParent) < boneCount)
            {
                XMMATRIX parentWorldInv = XMMatrixInverse(nullptr, globalTransforms[midParent]);
                XMMATRIX newGlobal = midDeltaRot * globalTransforms[midIdx];
                XMMATRIX newLocal = newGlobal * parentWorldInv;
                newLocal.r[3] = midLocal.r[3];
                XMStoreFloat4x4(&localTransforms[midIdx], newLocal);
            }
            else
            {
                XMMATRIX newLocal = midDeltaRot * midLocal;
                newLocal.r[3] = midLocal.r[3];
                XMStoreFloat4x4(&localTransforms[midIdx], newLocal);
            }
        }

        // Note: IK weight blending is handled by the caller (AnimationInstance::Update())
        // which saves pre-IK transforms and blends per-bone after solving. This allows
        // the solver to operate at full strength and the blend to be applied cleanly.
    }

    void AnimationEvaluator::SolveLookAtIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                           const IKChain& chain)
    {
        if (!chain.enabled || chain.boneIndices.empty())
            return;

        int32_t boneIdx = chain.boneIndices[0];
        if (boneIdx < 0 || boneIdx >= static_cast<int32_t>(localTransforms.size()))
            return;

        XMMATRIX local = XMLoadFloat4x4(&localTransforms[boneIdx]);
        XMVECTOR bonePos = local.r[3];
        XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
        XMVECTOR toTarget = XMVector3Normalize(XMVectorSubtract(target, bonePos));
        XMVECTOR forward = XMVectorSet(0, 0, 1, 0);

        XMVECTOR rotAxis = XMVector3Cross(forward, toTarget);
        float dot = XMVectorGetX(XMVector3Dot(forward, toTarget));
        dot = (std::max)(-1.0f, (std::min)(1.0f, dot));

        if (XMVectorGetX(XMVector3LengthSq(rotAxis)) > 0.0001f)
        {
            float angle = std::acos(dot) * chain.weight;
            XMMATRIX rotation = XMMatrixRotationAxis(XMVector3Normalize(rotAxis), angle);
            XMMATRIX translation = XMMatrixTranslationFromVector(bonePos);
            XMMATRIX newLocal = rotation * translation;
            XMStoreFloat4x4(&localTransforms[boneIdx], newLocal);
        }
    }

    void AnimationEvaluator::SolveFABRIK(std::vector<XMFLOAT4X4>& localTransforms, const Skeleton& skeleton,
                                         const IKChain& chain)
    {
        if (!chain.enabled || chain.boneIndices.size() < 2)
            return;

        const size_t numJoints = chain.boneIndices.size();

        // Validate all bone indices
        for (size_t i = 0; i < numJoints; ++i)
        {
            int32_t idx = chain.boneIndices[i];
            if (idx < 0 || idx >= static_cast<int32_t>(localTransforms.size()))
                return;
        }

        // Step 1: Compute global (world) transforms for all bones in the skeleton
        // so we can extract joint world positions
        size_t boneCount = skeleton.bones.size();
        std::vector<XMMATRIX> globalTransforms(boneCount);
        for (size_t i = 0; i < boneCount; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);
            int32_t parentIdx = skeleton.bones[i].parentIndex;

            if (parentIdx >= 0 && parentIdx < static_cast<int32_t>(boneCount))
            {
                globalTransforms[i] = local * globalTransforms[parentIdx];
            }
            else
            {
                globalTransforms[i] = local;
            }
        }

        // Step 2: Extract world positions of each joint in the IK chain
        std::vector<XMVECTOR> positions(numJoints);
        for (size_t i = 0; i < numJoints; ++i)
        {
            int32_t boneIdx = chain.boneIndices[i];
            positions[i] = globalTransforms[boneIdx].r[3];
        }

        // Step 3: Compute distances (bone lengths) between consecutive joints
        std::vector<float> boneLengths(numJoints - 1);
        float totalLength = 0.0f;
        for (size_t i = 0; i < numJoints - 1; ++i)
        {
            XMVECTOR diff = XMVectorSubtract(positions[i + 1], positions[i]);
            boneLengths[i] = XMVectorGetX(XMVector3Length(diff));
            totalLength += boneLengths[i];
        }

        XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
        XMVECTOR rootPos = positions[0];

        // Check if the target is reachable
        XMVECTOR rootToTarget = XMVectorSubtract(target, rootPos);
        float distToTarget = XMVectorGetX(XMVector3Length(rootToTarget));

        if (distToTarget > totalLength)
        {
            // Target is unreachable: stretch the chain toward the target
            XMVECTOR dir = XMVector3Normalize(rootToTarget);
            for (size_t i = 0; i < numJoints - 1; ++i)
            {
                positions[i + 1] = XMVectorAdd(positions[i], XMVectorScale(dir, boneLengths[i]));
            }
        }
        else
        {
            // FABRIK iterative solver
            for (int iteration = 0; iteration < chain.maxIterations; ++iteration)
            {
                // Check convergence: end effector close enough to target
                XMVECTOR endToTarget = XMVectorSubtract(target, positions[numJoints - 1]);
                float error = XMVectorGetX(XMVector3Length(endToTarget));
                if (error < chain.tolerance)
                    break;

                // Forward pass: move end effector to target, adjust chain toward root
                positions[numJoints - 1] = target;
                for (int i = static_cast<int>(numJoints) - 2; i >= 0; --i)
                {
                    XMVECTOR dir = XMVectorSubtract(positions[i], positions[i + 1]);
                    dir = XMVector3Normalize(dir);
                    positions[i] = XMVectorAdd(positions[i + 1], XMVectorScale(dir, boneLengths[i]));
                }

                // Backward pass: pin root, adjust chain toward end effector
                positions[0] = rootPos;
                for (size_t i = 0; i < numJoints - 1; ++i)
                {
                    XMVECTOR dir = XMVectorSubtract(positions[i + 1], positions[i]);
                    dir = XMVector3Normalize(dir);
                    positions[i + 1] = XMVectorAdd(positions[i], XMVectorScale(dir, boneLengths[i]));
                }
            }
        }

        // Step 4: Apply weight blending between original and solved positions
        if (chain.weight < 1.0f)
        {
            std::vector<XMVECTOR> origPositions(numJoints);
            for (size_t i = 0; i < numJoints; ++i)
            {
                int32_t boneIdx = chain.boneIndices[i];
                origPositions[i] = globalTransforms[boneIdx].r[3];
            }
            for (size_t i = 0; i < numJoints; ++i)
            {
                positions[i] = XMVectorLerp(origPositions[i], positions[i], chain.weight);
            }
        }

        // Step 5: Update bone local transforms from the solved world positions.
        // For each joint, compute the rotation needed to point from this joint
        // to the next joint in the new configuration, and update the local transform.
        for (size_t i = 0; i < numJoints - 1; ++i)
        {
            int32_t boneIdx = chain.boneIndices[i];
            int32_t childIdx = chain.boneIndices[i + 1];

            // Compute original direction from this joint to the next in world space
            XMVECTOR origDir =
                XMVector3Normalize(XMVectorSubtract(globalTransforms[childIdx].r[3], globalTransforms[boneIdx].r[3]));

            // Compute new direction from solved positions
            XMVECTOR newDir = XMVector3Normalize(XMVectorSubtract(positions[i + 1], positions[i]));

            // Compute rotation from original direction to new direction
            XMVECTOR rotAxis = XMVector3Cross(origDir, newDir);
            float dotProduct = XMVectorGetX(XMVector3Dot(origDir, newDir));
            dotProduct = (std::max)(-1.0f, (std::min)(1.0f, dotProduct));

            if (XMVectorGetX(XMVector3LengthSq(rotAxis)) > 1e-8f)
            {
                float angle = std::acos(dotProduct);
                XMMATRIX rotation = XMMatrixRotationAxis(XMVector3Normalize(rotAxis), angle);

                // Apply the world-space rotation to the bone's local transform
                XMMATRIX local = XMLoadFloat4x4(&localTransforms[boneIdx]);

                // Decompose local to preserve translation and scale
                XMVECTOR localPos = local.r[3];
                XMMATRIX rotatedLocal = local * rotation;
                // Restore the original local translation (position stays the same)
                rotatedLocal.r[3] = localPos;

                XMStoreFloat4x4(&localTransforms[boneIdx], rotatedLocal);
            }
        }
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

        // Track whether the base layer has been established (used for debugging)
        [[maybe_unused]] bool baseLayerSet = false;

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
                baseLayerSet = true;
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
        stateMachine.Update(deltaTime);

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
