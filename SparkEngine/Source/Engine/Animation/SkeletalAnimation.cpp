/**
 * @file SkeletalAnimation.cpp
 * @brief Low-level skeletal animation evaluation — keyframe interpolation, clip sampling,
 *        transform blending, and skinning matrix computation
 *
 * Extracted from AnimationSystem.cpp to keep each file focused on a single responsibility.
 */
#include "../../Core/Platform.h"
#include "AnimationSystem.h"
#include <cmath>

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
    // AnimationEvaluator — Clip Sampling, Blending, Skinning
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

} // namespace Spark::Animation
