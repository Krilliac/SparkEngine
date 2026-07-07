/**
 * @file Test_tests_inversekinematics.cpp
 * @brief Real-class unit tests for the IK solvers in AnimationEvaluator
 *        (SolveTwoBoneIK, SolveFABRIK, SolveLookAtIK).
 *
 * These exercise the shipped static solvers directly (no reimplementation):
 *  - guard clauses: degenerate / out-of-range / negative bone indices and
 *    too-short chains must leave localTransforms byte-for-byte untouched;
 *  - LookAt aligns the bone's forward (+Z) basis with the target direction;
 *  - FABRIK and TwoBone reduce end-effector error toward a reachable target
 *    and stretch toward an unreachable one.
 */

#include "TestFramework.h"
#include "Engine/Animation/AnimationSystem.h"

#include <vector>

using namespace Spark::Animation;
using namespace DirectX;

namespace
{
    XMFLOAT4X4 IdentityMat()
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixIdentity());
        return m;
    }

    XMFLOAT4X4 TranslationMat(float x, float y, float z)
    {
        XMFLOAT4X4 m;
        XMStoreFloat4x4(&m, XMMatrixTranslation(x, y, z));
        return m;
    }

    bool Mat4Equal(const XMFLOAT4X4& a, const XMFLOAT4X4& b)
    {
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                if (a.m[r][c] != b.m[r][c])
                    return false;
            }
        }
        return true;
    }

    bool AllEqual(const std::vector<XMFLOAT4X4>& a, const std::vector<XMFLOAT4X4>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (!Mat4Equal(a[i], b[i]))
                return false;
        }
        return true;
    }

    // A straight chain of `count` bones; bone i is the parent of bone i+1.
    Skeleton MakeChainSkeleton(size_t count)
    {
        Skeleton s;
        s.bones.resize(count);
        for (size_t i = 0; i < count; ++i)
            s.bones[i].parentIndex = (i == 0) ? -1 : static_cast<int32_t>(i - 1);
        return s;
    }

    // Local transforms for a straight chain laid out along +X: each child sits
    // one unit further along X than its parent, so world positions are
    // (0,0,0), (1,0,0), (2,0,0), ...
    std::vector<XMFLOAT4X4> MakeStraightChainLocals(size_t count)
    {
        std::vector<XMFLOAT4X4> locals(count);
        locals[0] = IdentityMat();
        for (size_t i = 1; i < count; ++i)
            locals[i] = TranslationMat(1.0f, 0.0f, 0.0f);
        return locals;
    }

    XMFLOAT3 BoneWorldPos(const std::vector<XMFLOAT4X4>& locals, const Skeleton& skel, int32_t idx)
    {
        const size_t n = skel.bones.size();
        std::vector<XMMATRIX> global(n);
        for (size_t i = 0; i < n; ++i)
        {
            XMMATRIX local = XMLoadFloat4x4(&locals[i]);
            int32_t p = skel.bones[i].parentIndex;
            global[i] = (p >= 0 && static_cast<size_t>(p) < n) ? local * global[p] : local;
        }
        XMFLOAT3 out;
        XMStoreFloat3(&out, global[idx].r[3]);
        return out;
    }

    float Dist(const XMFLOAT3& a, const XMFLOAT3& b)
    {
        XMVECTOR va = XMLoadFloat3(&a);
        XMVECTOR vb = XMLoadFloat3(&b);
        return XMVectorGetX(XMVector3Length(XMVectorSubtract(va, vb)));
    }
} // namespace

// ---------------------------------------------------------------------------
// Guard clauses — must not touch localTransforms
// ---------------------------------------------------------------------------

TEST(IK_TwoBone_OutOfRangeIndicesNoOp)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);
    std::vector<XMFLOAT4X4> before = locals;

    IKChain chain;
    chain.type = IKType::TwoBone;
    chain.enabled = true;
    chain.boneIndices = {0, 1, 5}; // 5 is out of range
    chain.targetPosition = {1.0f, 1.0f, 0.0f};

    AnimationEvaluator::SolveTwoBoneIK(locals, skel, chain);
    EXPECT_TRUE(AllEqual(locals, before));
}

TEST(IK_TwoBone_NegativeIndexNoOp)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);
    std::vector<XMFLOAT4X4> before = locals;

    IKChain chain;
    chain.type = IKType::TwoBone;
    chain.enabled = true;
    chain.boneIndices = {-1, 1, 2};
    chain.targetPosition = {1.0f, 1.0f, 0.0f};

    AnimationEvaluator::SolveTwoBoneIK(locals, skel, chain);
    EXPECT_TRUE(AllEqual(locals, before));
}

TEST(IK_TwoBone_ShortChainNoOp)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);
    std::vector<XMFLOAT4X4> before = locals;

    IKChain chain;
    chain.type = IKType::TwoBone;
    chain.enabled = true;
    chain.boneIndices = {0, 1}; // fewer than 3 → no-op
    chain.targetPosition = {1.0f, 1.0f, 0.0f};

    AnimationEvaluator::SolveTwoBoneIK(locals, skel, chain);
    EXPECT_TRUE(AllEqual(locals, before));
}

TEST(IK_FABRIK_OutOfRangeIndicesNoOp)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);
    std::vector<XMFLOAT4X4> before = locals;

    IKChain chain;
    chain.type = IKType::FABRIK;
    chain.enabled = true;
    chain.boneIndices = {0, 5}; // 5 out of range
    chain.targetPosition = {1.0f, 1.0f, 0.0f};

    AnimationEvaluator::SolveFABRIK(locals, skel, chain);
    EXPECT_TRUE(AllEqual(locals, before));
}

TEST(IK_FABRIK_ShortChainNoOp)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);
    std::vector<XMFLOAT4X4> before = locals;

    IKChain chain;
    chain.type = IKType::FABRIK;
    chain.enabled = true;
    chain.boneIndices = {0}; // fewer than 2 → no-op
    chain.targetPosition = {1.0f, 1.0f, 0.0f};

    AnimationEvaluator::SolveFABRIK(locals, skel, chain);
    EXPECT_TRUE(AllEqual(locals, before));
}

TEST(IK_LookAt_OutOfRangeIndexNoOp)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);
    std::vector<XMFLOAT4X4> before = locals;

    IKChain chain;
    chain.type = IKType::LookAt;
    chain.enabled = true;
    chain.boneIndices = {5}; // out of range
    chain.targetPosition = {1.0f, 0.0f, 0.0f};

    AnimationEvaluator::SolveLookAtIK(locals, skel, chain);
    EXPECT_TRUE(AllEqual(locals, before));
}

// ---------------------------------------------------------------------------
// LookAt — bone forward (+Z) basis should point at the target
// ---------------------------------------------------------------------------

TEST(IK_LookAt_PointsForwardAtTarget)
{
    Skeleton skel = MakeChainSkeleton(1);
    std::vector<XMFLOAT4X4> locals(1, IdentityMat());

    IKChain chain;
    chain.type = IKType::LookAt;
    chain.enabled = true;
    chain.weight = 1.0f;
    chain.boneIndices = {0};
    chain.targetPosition = {1.0f, 0.0f, 0.0f}; // straight along +X from the origin

    AnimationEvaluator::SolveLookAtIK(locals, skel, chain);

    // The local Z basis (row 2) should now align with the +X target direction.
    XMMATRIX m = XMLoadFloat4x4(&locals[0]);
    XMVECTOR forward = XMVector3Normalize(m.r[2]);
    XMFLOAT3 f;
    XMStoreFloat3(&f, forward);
    EXPECT_NEAR(f.x, 1.0f, 0.01f);
    EXPECT_NEAR(f.y, 0.0f, 0.01f);
    EXPECT_NEAR(f.z, 0.0f, 0.01f);
}

// ---------------------------------------------------------------------------
// FABRIK / TwoBone — error reduction toward the target
// ---------------------------------------------------------------------------

TEST(IK_FABRIK_ReachableReducesError)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);

    IKChain chain;
    chain.type = IKType::FABRIK;
    chain.enabled = true;
    chain.boneIndices = {0, 1, 2};
    chain.targetPosition = {1.0f, 1.0f, 0.0f}; // dist ~1.41 < reach 2

    float before = Dist(BoneWorldPos(locals, skel, 2), chain.targetPosition);
    AnimationEvaluator::SolveFABRIK(locals, skel, chain);
    float after = Dist(BoneWorldPos(locals, skel, 2), chain.targetPosition);

    EXPECT_LT(after, before);
}

TEST(IK_FABRIK_UnreachableStretchesTowardTarget)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);

    IKChain chain;
    chain.type = IKType::FABRIK;
    chain.enabled = true;
    chain.boneIndices = {0, 1, 2};
    chain.targetPosition = {0.0f, 10.0f, 0.0f}; // far beyond reach 2

    float before = Dist(BoneWorldPos(locals, skel, 2), chain.targetPosition);
    AnimationEvaluator::SolveFABRIK(locals, skel, chain);
    float after = Dist(BoneWorldPos(locals, skel, 2), chain.targetPosition);

    // Unreachable: the chain straightens toward the target, so the end
    // effector gets closer even though it cannot arrive.
    EXPECT_LT(after, before);
}

TEST(IK_TwoBone_ReachableReducesError)
{
    Skeleton skel = MakeChainSkeleton(3);
    std::vector<XMFLOAT4X4> locals = MakeStraightChainLocals(3);

    IKChain chain;
    chain.type = IKType::TwoBone;
    chain.enabled = true;
    chain.boneIndices = {0, 1, 2};
    chain.poleVector = {0.0f, 1.0f, 0.0f};
    chain.targetPosition = {1.0f, 1.0f, 0.0f}; // reachable

    float before = Dist(BoneWorldPos(locals, skel, 2), chain.targetPosition);
    AnimationEvaluator::SolveTwoBoneIK(locals, skel, chain);
    float after = Dist(BoneWorldPos(locals, skel, 2), chain.targetPosition);

    EXPECT_LT(after, before);
}
