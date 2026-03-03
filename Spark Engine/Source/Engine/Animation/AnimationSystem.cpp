/**
 * @file AnimationSystem.cpp
 * @brief Implementation of skeletal animation system
 */

#include "AnimationSystem.h"
#include <sstream>
#include <cmath>

namespace Spark::Animation {

// ============================================================================
// BoneAnimation — Keyframe Interpolation
// ============================================================================

template<typename T>
static int FindKeyIndex(const std::vector<T>& keys, float time) {
    for (int i = 0; i < static_cast<int>(keys.size()) - 1; ++i) {
        if (time < keys[i + 1].time)
            return i;
    }
    return static_cast<int>(keys.size()) - 1;
}

XMFLOAT3 BoneAnimation::InterpolatePosition(float time) const {
    if (positionKeys.empty()) return {0, 0, 0};
    if (positionKeys.size() == 1) return positionKeys[0].value;

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

XMFLOAT4 BoneAnimation::InterpolateRotation(float time) const {
    if (rotationKeys.empty()) return {0, 0, 0, 1};
    if (rotationKeys.size() == 1) return rotationKeys[0].value;

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

XMFLOAT3 BoneAnimation::InterpolateScale(float time) const {
    if (scaleKeys.empty()) return {1, 1, 1};
    if (scaleKeys.size() == 1) return scaleKeys[0].value;

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

void AnimationStateMachine::AddState(const AnimationState& state) {
    m_states[state.name] = state;
    if (m_defaultState.empty())
        m_defaultState = state.name;
    if (m_currentState.empty())
        m_currentState = state.name;
}

void AnimationStateMachine::AddTransition(const AnimationTransition& transition) {
    m_transitions.push_back(transition);
}

void AnimationStateMachine::SetDefaultState(const std::string& stateName) {
    m_defaultState = stateName;
    if (m_currentState.empty())
        m_currentState = stateName;
}

void AnimationStateMachine::Update(float deltaTime) {
    if (m_isTransitioning) {
        m_transitionElapsed += deltaTime;
        m_blendFactor = (m_transitionDuration > 0.0f)
            ? (std::min)(m_transitionElapsed / m_transitionDuration, 1.0f)
            : 1.0f;

        if (m_blendFactor >= 1.0f) {
            m_currentState = m_targetState;
            m_isTransitioning = false;
            m_blendFactor = 0.0f;
            m_currentTime = m_transitionElapsed;
        }
        return;
    }

    // Check transitions
    for (const auto& t : m_transitions) {
        if (t.fromState == m_currentState && t.condition && t.condition()) {
            m_targetState = t.toState;
            m_transitionDuration = t.duration;
            m_transitionElapsed = 0.0f;
            m_isTransitioning = true;
            m_blendFactor = 0.0f;
            break;
        }
    }

    auto it = m_states.find(m_currentState);
    if (it != m_states.end()) {
        m_currentTime += deltaTime * it->second.speed;
    }
}

void AnimationStateMachine::ForceState(const std::string& stateName) {
    if (m_states.find(stateName) != m_states.end()) {
        m_currentState = stateName;
        m_isTransitioning = false;
        m_currentTime = 0.0f;
        m_blendFactor = 0.0f;
    }
}

std::string AnimationStateMachine::Console_GetStateInfo() const {
    std::ostringstream ss;
    ss << "Current State: " << m_currentState << "\n";
    ss << "Time: " << m_currentTime << "s\n";
    if (m_isTransitioning) {
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

AnimationManager& AnimationManager::GetInstance() {
    static AnimationManager instance;
    return instance;
}

std::shared_ptr<Skeleton> AnimationManager::LoadSkeleton(const std::string& filepath) {
    auto it = m_skeletons.find(filepath);
    if (it != m_skeletons.end())
        return it->second;

    // Create skeleton structure — actual loading would use Assimp
    auto skeleton = std::make_shared<Skeleton>();
    skeleton->name = filepath;
    m_skeletons[filepath] = skeleton;
    return skeleton;
}

std::vector<std::shared_ptr<AnimationClip>> AnimationManager::LoadAnimations(const std::string& filepath) {
    std::vector<std::shared_ptr<AnimationClip>> clips;
    // Actual loading would parse FBX/glTF via Assimp and extract animation data
    // For now, return empty — integration point for asset pipeline
    return clips;
}

void AnimationManager::RegisterClip(const std::string& name, std::shared_ptr<AnimationClip> clip) {
    m_clips[name] = std::move(clip);
}

std::shared_ptr<AnimationClip> AnimationManager::GetClip(const std::string& name) const {
    auto it = m_clips.find(name);
    return (it != m_clips.end()) ? it->second : nullptr;
}

std::shared_ptr<Skeleton> AnimationManager::GetSkeleton(const std::string& name) const {
    auto it = m_skeletons.find(name);
    return (it != m_skeletons.end()) ? it->second : nullptr;
}

void AnimationManager::Clear() {
    m_clips.clear();
    m_skeletons.clear();
}

std::string AnimationManager::Console_ListAnimations() const {
    std::ostringstream ss;
    ss << "=== Loaded Animations (" << m_clips.size() << ") ===\n";
    for (const auto& [name, clip] : m_clips) {
        ss << "  " << name << " [" << clip->duration << "s, "
           << clip->channels.size() << " channels]\n";
    }
    return ss.str();
}

std::string AnimationManager::Console_ListSkeletons() const {
    std::ostringstream ss;
    ss << "=== Loaded Skeletons (" << m_skeletons.size() << ") ===\n";
    for (const auto& [name, skel] : m_skeletons) {
        ss << "  " << name << " [" << skel->bones.size() << " bones]\n";
    }
    return ss.str();
}

// ============================================================================
// AnimationEvaluator
// ============================================================================

void AnimationEvaluator::SampleClip(const AnimationClip& clip, const Skeleton& skeleton,
                                     float time, std::vector<XMFLOAT4X4>& outLocalTransforms) {
    size_t boneCount = skeleton.bones.size();
    outLocalTransforms.resize(boneCount);

    // Initialize with bind pose
    for (size_t i = 0; i < boneCount; ++i)
        outLocalTransforms[i] = skeleton.bones[i].localBindPose;

    // Wrap time for looping
    float animTime = time;
    if (clip.duration > 0.0f && clip.loop) {
        animTime = std::fmod(time, clip.duration);
        if (animTime < 0.0f) animTime += clip.duration;
    }

    // Sample each channel
    for (const auto& channel : clip.channels) {
        int32_t boneIdx = channel.boneIndex;
        if (boneIdx < 0) {
            boneIdx = skeleton.FindBone(channel.boneName);
            if (boneIdx < 0) continue;
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

void AnimationEvaluator::BlendTransforms(const std::vector<XMFLOAT4X4>& a,
                                          const std::vector<XMFLOAT4X4>& b,
                                          float blendFactor,
                                          std::vector<XMFLOAT4X4>& outResult) {
    size_t count = (std::min)(a.size(), b.size());
    outResult.resize(count);

    for (size_t i = 0; i < count; ++i) {
        XMMATRIX mA = XMLoadFloat4x4(&a[i]);
        XMMATRIX mB = XMLoadFloat4x4(&b[i]);

        // Decompose, interpolate, recompose
        XMVECTOR sA, rA, tA, sB, rB, tB;
        XMMatrixDecompose(&sA, &rA, &tA, mA);
        XMMatrixDecompose(&sB, &rB, &tB, mB);

        XMVECTOR s = XMVectorLerp(sA, sB, blendFactor);
        XMVECTOR r = XMQuaternionSlerp(rA, rB, blendFactor);
        XMVECTOR t = XMVectorLerp(tA, tB, blendFactor);

        XMMATRIX result = XMMatrixScalingFromVector(s) *
                          XMMatrixRotationQuaternion(r) *
                          XMMatrixTranslationFromVector(t);
        XMStoreFloat4x4(&outResult[i], result);
    }
}

void AnimationEvaluator::ComputeSkinningMatrices(const Skeleton& skeleton,
                                                  const std::vector<XMFLOAT4X4>& localTransforms,
                                                  std::vector<XMFLOAT4X4>& outFinalTransforms) {
    size_t boneCount = skeleton.bones.size();
    outFinalTransforms.resize(boneCount);

    // Compute global transforms by traversing hierarchy
    std::vector<XMMATRIX> globalTransforms(boneCount);

    for (size_t i = 0; i < boneCount; ++i) {
        XMMATRIX local = XMLoadFloat4x4(&localTransforms[i]);

        if (skeleton.bones[i].parentIndex >= 0) {
            globalTransforms[i] = local * globalTransforms[skeleton.bones[i].parentIndex];
        } else {
            globalTransforms[i] = local;
        }
    }

    // Multiply by offset (inverse bind pose) to get final skinning matrices
    for (size_t i = 0; i < boneCount; ++i) {
        XMMATRIX offset = XMLoadFloat4x4(&skeleton.bones[i].offsetMatrix);
        XMMATRIX finalMat = offset * globalTransforms[i];
        XMStoreFloat4x4(&outFinalTransforms[i], finalMat);
    }
}

void AnimationEvaluator::SolveTwoBoneIK(std::vector<XMFLOAT4X4>& localTransforms,
                                          const Skeleton& skeleton,
                                          const IKChain& chain) {
    if (!chain.enabled || chain.boneIndices.size() < 3) return;

    int32_t rootIdx = chain.boneIndices[0];
    int32_t midIdx = chain.boneIndices[1];
    int32_t endIdx = chain.boneIndices[2];

    if (rootIdx < 0 || midIdx < 0 || endIdx < 0) return;
    if (rootIdx >= static_cast<int32_t>(localTransforms.size())) return;

    // Compute world positions for the three bones
    // For a full implementation, we'd need global transforms here
    // This is a simplified version that works with local transforms
    XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
    XMVECTOR pole = XMLoadFloat3(&chain.poleVector);

    XMMATRIX rootLocal = XMLoadFloat4x4(&localTransforms[rootIdx]);
    XMMATRIX midLocal = XMLoadFloat4x4(&localTransforms[midIdx]);

    // Extract positions
    XMVECTOR rootPos = rootLocal.r[3];
    XMVECTOR midPos = midLocal.r[3];

    // Compute chain lengths
    XMVECTOR rootToMid = XMVectorSubtract(midPos, rootPos);
    float upperLen = XMVectorGetX(XMVector3Length(rootToMid));

    // Apply IK rotation to root and mid bones
    XMVECTOR rootToTarget = XMVectorSubtract(target, rootPos);
    float targetDist = XMVectorGetX(XMVector3Length(rootToTarget));

    if (targetDist > 0.001f && upperLen > 0.001f) {
        XMVECTOR dir = XMVector3Normalize(rootToTarget);
        XMVECTOR currentDir = XMVector3Normalize(rootToMid);

        XMVECTOR rotAxis = XMVector3Cross(currentDir, dir);
        float dotProduct = XMVectorGetX(XMVector3Dot(currentDir, dir));
        dotProduct = (std::max)(-1.0f, (std::min)(1.0f, dotProduct));
        float angle = std::acos(dotProduct) * chain.weight;

        if (XMVectorGetX(XMVector3LengthSq(rotAxis)) > 0.0001f) {
            XMMATRIX rotation = XMMatrixRotationAxis(XMVector3Normalize(rotAxis), angle);
            XMMATRIX newLocal = rootLocal * rotation;
            XMStoreFloat4x4(&localTransforms[rootIdx], newLocal);
        }
    }
}

void AnimationEvaluator::SolveLookAtIK(std::vector<XMFLOAT4X4>& localTransforms,
                                         const Skeleton& skeleton,
                                         const IKChain& chain) {
    if (!chain.enabled || chain.boneIndices.empty()) return;

    int32_t boneIdx = chain.boneIndices[0];
    if (boneIdx < 0 || boneIdx >= static_cast<int32_t>(localTransforms.size())) return;

    XMMATRIX local = XMLoadFloat4x4(&localTransforms[boneIdx]);
    XMVECTOR bonePos = local.r[3];
    XMVECTOR target = XMLoadFloat3(&chain.targetPosition);
    XMVECTOR toTarget = XMVector3Normalize(XMVectorSubtract(target, bonePos));
    XMVECTOR forward = XMVectorSet(0, 0, 1, 0);

    XMVECTOR rotAxis = XMVector3Cross(forward, toTarget);
    float dot = XMVectorGetX(XMVector3Dot(forward, toTarget));
    dot = (std::max)(-1.0f, (std::min)(1.0f, dot));

    if (XMVectorGetX(XMVector3LengthSq(rotAxis)) > 0.0001f) {
        float angle = std::acos(dot) * chain.weight;
        XMMATRIX rotation = XMMatrixRotationAxis(XMVector3Normalize(rotAxis), angle);
        XMMATRIX translation = XMMatrixTranslationFromVector(bonePos);
        XMMATRIX newLocal = rotation * translation;
        XMStoreFloat4x4(&localTransforms[boneIdx], newLocal);
    }
}

} // namespace Spark::Animation
