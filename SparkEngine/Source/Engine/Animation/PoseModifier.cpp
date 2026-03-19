/**
 * @file PoseModifier.cpp
 * @brief Implementation of CryEngine-inspired pose modification pipeline
 * @author Spark Engine Team
 * @date 2026
 */

#include "PoseModifier.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::Animation
{

    // =========================================================================
    // Quaternion Helpers (local to this translation unit)
    // =========================================================================

    namespace
    {

        /** @brief Compute quaternion dot product. */
        float QuatDot(const XMFLOAT4& a, const XMFLOAT4& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        }

        /** @brief Normalize a quaternion. */
        XMFLOAT4 QuatNormalize(const XMFLOAT4& q)
        {
            float len = std::sqrt(QuatDot(q, q));
            if (len < 0.0001f)
            {
                return {0.0f, 0.0f, 0.0f, 1.0f};
            }
            float inv = 1.0f / len;
            return {q.x * inv, q.y * inv, q.z * inv, q.w * inv};
        }

        /** @brief Spherical linear interpolation between two quaternions. */
        XMFLOAT4 QuatSlerp(const XMFLOAT4& a, const XMFLOAT4& b, float t)
        {
            float dot = QuatDot(a, b);

            // Ensure shortest path
            XMFLOAT4 b2 = b;
            if (dot < 0.0f)
            {
                dot = -dot;
                b2 = {-b.x, -b.y, -b.z, -b.w};
            }

            // If very close, use linear interpolation to avoid numerical issues
            if (dot > 0.9995f)
            {
                return QuatNormalize(
                    {a.x + (b2.x - a.x) * t, a.y + (b2.y - a.y) * t, a.z + (b2.z - a.z) * t, a.w + (b2.w - a.w) * t});
            }

            float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
            float sinTheta = std::sin(theta);
            if (sinTheta < 0.0001f)
            {
                return a;
            }

            float wa = std::sin((1.0f - t) * theta) / sinTheta;
            float wb = std::sin(t * theta) / sinTheta;

            return {a.x * wa + b2.x * wb, a.y * wa + b2.y * wb, a.z * wa + b2.z * wb, a.w * wa + b2.w * wb};
        }

        /** @brief Multiply two quaternions (a * b). */
        XMFLOAT4 QuatMultiply(const XMFLOAT4& a, const XMFLOAT4& b)
        {
            return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                    a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        }

        /** @brief Create a quaternion from axis (normalized) and angle (radians). */
        XMFLOAT4 QuatFromAxisAngle(const XMFLOAT3& axis, float angle)
        {
            float halfAngle = angle * 0.5f;
            float s = std::sin(halfAngle);
            return {axis.x * s, axis.y * s, axis.z * s, std::cos(halfAngle)};
        }

        /** @brief Compute the length of a 3D vector. */
        float Vec3Length(const XMFLOAT3& v)
        {
            return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
        }

        /** @brief Normalize a 3D vector (returns up vector if degenerate). */
        XMFLOAT3 Vec3Normalize(const XMFLOAT3& v)
        {
            float len = Vec3Length(v);
            if (len < 0.0001f)
            {
                return {0.0f, 1.0f, 0.0f};
            }
            float inv = 1.0f / len;
            return {v.x * inv, v.y * inv, v.z * inv};
        }

        /** @brief Cross product of two 3D vectors. */
        XMFLOAT3 Vec3Cross(const XMFLOAT3& a, const XMFLOAT3& b)
        {
            return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
        }

        /** @brief Dot product of two 3D vectors. */
        float Vec3Dot(const XMFLOAT3& a, const XMFLOAT3& b)
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

    } // anonymous namespace

    // =========================================================================
    // LookAtModifier
    // =========================================================================

    void LookAtModifier::Apply(PoseContext& pose)
    {
        if (m_boneIndex < 0 || static_cast<size_t>(m_boneIndex) >= pose.boneTransforms.size())
        {
            return;
        }

        auto& bone = pose.boneTransforms[m_boneIndex];

        // Direction from bone position to target
        XMFLOAT3 toTarget = {m_target.x - bone.position.x, m_target.y - bone.position.y, m_target.z - bone.position.z};

        float dist = Vec3Length(toTarget);
        if (dist < 0.001f)
        {
            return;
        }

        XMFLOAT3 dirToTarget = Vec3Normalize(toTarget);

        // Assume bone's forward axis is +Z in its local space
        XMFLOAT3 forward = {0.0f, 0.0f, 1.0f};

        // Compute rotation axis and angle from forward to target direction
        XMFLOAT3 axis = Vec3Cross(forward, dirToTarget);
        float axisLen = Vec3Length(axis);

        if (axisLen < 0.0001f)
        {
            // Already facing target (or 180 degrees)
            return;
        }

        axis = Vec3Normalize(axis);
        float dotProduct = std::clamp(Vec3Dot(forward, dirToTarget), -1.0f, 1.0f);
        float angle = std::acos(dotProduct);

        // Clamp to max angle
        angle = std::min(angle, m_maxAngle);

        // Create rotation quaternion and blend with weight
        XMFLOAT4 lookRotation = QuatFromAxisAngle(axis, angle);
        XMFLOAT4 identity = {0.0f, 0.0f, 0.0f, 1.0f};
        XMFLOAT4 blendedRotation = QuatSlerp(identity, lookRotation, m_weight);

        // Apply on top of existing rotation
        bone.rotation = QuatNormalize(QuatMultiply(blendedRotation, bone.rotation));
    }

    // =========================================================================
    // AimIKModifier
    // =========================================================================

    void AimIKModifier::SetBoneChain(int32_t shoulder, int32_t elbow, int32_t hand)
    {
        m_shoulderIndex = shoulder;
        m_elbowIndex = elbow;
        m_handIndex = hand;
    }

    void AimIKModifier::Apply(PoseContext& pose)
    {
        // Validate bone indices
        auto boneCount = static_cast<int32_t>(pose.boneTransforms.size());
        if (m_shoulderIndex < 0 || m_shoulderIndex >= boneCount || m_elbowIndex < 0 || m_elbowIndex >= boneCount ||
            m_handIndex < 0 || m_handIndex >= boneCount)
        {
            return;
        }

        auto& hand = pose.boneTransforms[m_handIndex];

        // Direction from hand to aim target
        XMFLOAT3 toTarget = {m_target.x - hand.position.x, m_target.y - hand.position.y, m_target.z - hand.position.z};

        float dist = Vec3Length(toTarget);
        if (dist < 0.001f)
        {
            return;
        }

        XMFLOAT3 aimDir = Vec3Normalize(toTarget);
        XMFLOAT3 forward = {0.0f, 0.0f, 1.0f};

        // Compute the rotation needed to align the hand forward with aim direction
        XMFLOAT3 axis = Vec3Cross(forward, aimDir);
        float axisLen = Vec3Length(axis);

        if (axisLen < 0.0001f)
        {
            return;
        }

        axis = Vec3Normalize(axis);
        float dotProduct = std::clamp(Vec3Dot(forward, aimDir), -1.0f, 1.0f);
        float angle = std::acos(dotProduct);

        // Distribute rotation across the chain: shoulder gets 40%, elbow 30%, hand 30%
        XMFLOAT4 identity = {0.0f, 0.0f, 0.0f, 1.0f};

        float shoulderAngle = angle * 0.4f * m_weight;
        float elbowAngle = angle * 0.3f * m_weight;
        float handAngle = angle * 0.3f * m_weight;

        XMFLOAT4 shoulderRot = QuatSlerp(identity, QuatFromAxisAngle(axis, shoulderAngle), 1.0f);
        XMFLOAT4 elbowRot = QuatSlerp(identity, QuatFromAxisAngle(axis, elbowAngle), 1.0f);
        XMFLOAT4 handRot = QuatSlerp(identity, QuatFromAxisAngle(axis, handAngle), 1.0f);

        auto& shoulder = pose.boneTransforms[m_shoulderIndex];
        auto& elbow = pose.boneTransforms[m_elbowIndex];

        shoulder.rotation = QuatNormalize(QuatMultiply(shoulderRot, shoulder.rotation));
        elbow.rotation = QuatNormalize(QuatMultiply(elbowRot, elbow.rotation));
        hand.rotation = QuatNormalize(QuatMultiply(handRot, hand.rotation));
    }

    // =========================================================================
    // FootPlacementModifier
    // =========================================================================

    void FootPlacementModifier::SetGroundHeights(float leftGroundY, float rightGroundY)
    {
        m_leftGroundY = leftGroundY;
        m_rightGroundY = rightGroundY;
    }

    void FootPlacementModifier::Apply(PoseContext& pose)
    {
        auto boneCount = static_cast<int32_t>(pose.boneTransforms.size());

        bool hasLeftFoot = (m_leftFootIndex >= 0 && m_leftFootIndex < boneCount);
        bool hasRightFoot = (m_rightFootIndex >= 0 && m_rightFootIndex < boneCount);
        bool hasPelvis = (m_pelvisIndex >= 0 && m_pelvisIndex < boneCount);

        if (!hasLeftFoot && !hasRightFoot)
        {
            return;
        }

        float leftDelta = 0.0f;
        float rightDelta = 0.0f;

        // Compute required vertical offsets for each foot
        if (hasLeftFoot)
        {
            float currentY = pose.boneTransforms[m_leftFootIndex].position.y;
            leftDelta = (m_leftGroundY - currentY) * m_weight;
            leftDelta = std::clamp(leftDelta, -m_maxAdjustment, m_maxAdjustment);
        }

        if (hasRightFoot)
        {
            float currentY = pose.boneTransforms[m_rightFootIndex].position.y;
            rightDelta = (m_rightGroundY - currentY) * m_weight;
            rightDelta = std::clamp(rightDelta, -m_maxAdjustment, m_maxAdjustment);
        }

        // Lower pelvis by the larger foot offset to prevent leg stretching
        if (hasPelvis)
        {
            float pelvisOffset = std::min(leftDelta, rightDelta);
            pose.boneTransforms[m_pelvisIndex].position.y += pelvisOffset;
        }

        // Apply foot offsets
        if (hasLeftFoot)
        {
            pose.boneTransforms[m_leftFootIndex].position.y += leftDelta;
        }

        if (hasRightFoot)
        {
            pose.boneTransforms[m_rightFootIndex].position.y += rightDelta;
        }
    }

    // =========================================================================
    // PoseModifierStack
    // =========================================================================

    void PoseModifierStack::AddModifier(std::unique_ptr<IPoseModifier> modifier)
    {
        if (modifier)
        {
            m_modifiers.push_back(std::move(modifier));
            m_needsSort = true;
        }
    }

    bool PoseModifierStack::RemoveModifier(const char* name)
    {
        auto it =
            std::remove_if(m_modifiers.begin(), m_modifiers.end(), [name](const std::unique_ptr<IPoseModifier>& mod)
                           { return std::strcmp(mod->GetName(), name) == 0; });

        if (it != m_modifiers.end())
        {
            m_modifiers.erase(it, m_modifiers.end());
            return true;
        }

        return false;
    }

    void PoseModifierStack::Apply(PoseContext& pose)
    {
        if (m_needsSort)
        {
            std::sort(m_modifiers.begin(), m_modifiers.end(),
                      [](const std::unique_ptr<IPoseModifier>& a, const std::unique_ptr<IPoseModifier>& b)
                      { return a->GetPriority() < b->GetPriority(); });
            m_needsSort = false;
        }

        for (auto& modifier : m_modifiers)
        {
            modifier->Apply(pose);
        }
    }

    void PoseModifierStack::Clear()
    {
        m_modifiers.clear();
        m_needsSort = false;
    }

} // namespace Spark::Animation
