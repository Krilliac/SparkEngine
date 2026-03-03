// TestAnimationSystem.cpp - Tests for skeletal animation system
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <cmath>
#include <algorithm>

namespace TestAnim {

// ============================================================================
// Minimal math types mirroring DirectXMath for cross-platform testing
// ============================================================================

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;

    Vec3() = default;
    Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

    float Length() const { return std::sqrt(x * x + y * y + z * z); }

    bool Near(const Vec3& o, float tol = 0.001f) const {
        return std::abs(x - o.x) <= tol &&
               std::abs(y - o.y) <= tol &&
               std::abs(z - o.z) <= tol;
    }
};

Vec3 Lerp(const Vec3& a, const Vec3& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
}

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

    Quat() = default;
    Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

    bool Near(const Quat& o, float tol = 0.001f) const {
        return std::abs(x - o.x) <= tol &&
               std::abs(y - o.y) <= tol &&
               std::abs(z - o.z) <= tol &&
               std::abs(w - o.w) <= tol;
    }

    float Length() const { return std::sqrt(x * x + y * y + z * z + w * w); }

    Quat Normalized() const {
        float len = Length();
        if (len < 0.0001f) return {0, 0, 0, 1};
        return {x / len, y / len, z / len, w / len};
    }
};

/// Simple normalized lerp for quaternions (sufficient for testing)
Quat NLerp(const Quat& a, const Quat& b, float t) {
    // Check for shortest path
    float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    Quat b2 = b;
    if (dot < 0.0f) {
        b2 = {-b.x, -b.y, -b.z, -b.w};
    }
    Quat result{
        a.x + (b2.x - a.x) * t,
        a.y + (b2.y - a.y) * t,
        a.z + (b2.z - a.z) * t,
        a.w + (b2.w - a.w) * t
    };
    return result.Normalized();
}

struct Mat4x4 {
    float m[4][4] = {};

    static Mat4x4 Identity() {
        Mat4x4 result{};
        result.m[0][0] = result.m[1][1] = result.m[2][2] = result.m[3][3] = 1.0f;
        return result;
    }

    bool Near(const Mat4x4& o, float tol = 0.001f) const {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                if (std::abs(m[r][c] - o.m[r][c]) > tol) return false;
        return true;
    }
};

Mat4x4 LerpMat4x4(const Mat4x4& a, const Mat4x4& b, float t) {
    Mat4x4 result{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            result.m[r][c] = a.m[r][c] + (b.m[r][c] - a.m[r][c]) * t;
    return result;
}

// ============================================================================
// Bone & Skeleton (mirrors Spark::Animation::Bone / Skeleton)
// ============================================================================

struct Bone {
    std::string name;
    int32_t parentIndex = -1;
    Mat4x4 offsetMatrix;
    Mat4x4 localBindPose;
};

struct Skeleton {
    std::string name;
    std::vector<Bone> bones;
    std::unordered_map<std::string, int32_t> boneNameToIndex;

    int32_t FindBone(const std::string& boneName) const {
        auto it = boneNameToIndex.find(boneName);
        return (it != boneNameToIndex.end()) ? it->second : -1;
    }

    size_t GetBoneCount() const { return bones.size(); }

    void AddBone(const std::string& boneName, int32_t parent) {
        int32_t idx = static_cast<int32_t>(bones.size());
        Bone b;
        b.name = boneName;
        b.parentIndex = parent;
        b.offsetMatrix = Mat4x4::Identity();
        b.localBindPose = Mat4x4::Identity();
        bones.push_back(b);
        boneNameToIndex[boneName] = idx;
    }
};

// ============================================================================
// Keyframes & Animation Clips
// ============================================================================

struct VectorKey {
    float time;
    Vec3 value;
};

struct QuatKey {
    float time;
    Quat value;
};

struct BoneAnimation {
    std::string boneName;
    int32_t boneIndex = -1;
    std::vector<VectorKey> positionKeys;
    std::vector<QuatKey> rotationKeys;
    std::vector<VectorKey> scaleKeys;

    Vec3 InterpolatePosition(float time) const {
        if (positionKeys.empty()) return Vec3{0, 0, 0};
        if (positionKeys.size() == 1 || time <= positionKeys.front().time)
            return positionKeys.front().value;
        if (time >= positionKeys.back().time)
            return positionKeys.back().value;

        for (size_t i = 0; i + 1 < positionKeys.size(); ++i) {
            if (time >= positionKeys[i].time && time <= positionKeys[i + 1].time) {
                float span = positionKeys[i + 1].time - positionKeys[i].time;
                float t = (span > 0.0f) ? (time - positionKeys[i].time) / span : 0.0f;
                return Lerp(positionKeys[i].value, positionKeys[i + 1].value, t);
            }
        }
        return positionKeys.back().value;
    }

    Quat InterpolateRotation(float time) const {
        if (rotationKeys.empty()) return Quat{0, 0, 0, 1};
        if (rotationKeys.size() == 1 || time <= rotationKeys.front().time)
            return rotationKeys.front().value;
        if (time >= rotationKeys.back().time)
            return rotationKeys.back().value;

        for (size_t i = 0; i + 1 < rotationKeys.size(); ++i) {
            if (time >= rotationKeys[i].time && time <= rotationKeys[i + 1].time) {
                float span = rotationKeys[i + 1].time - rotationKeys[i].time;
                float t = (span > 0.0f) ? (time - rotationKeys[i].time) / span : 0.0f;
                return NLerp(rotationKeys[i].value, rotationKeys[i + 1].value, t);
            }
        }
        return rotationKeys.back().value;
    }

    Vec3 InterpolateScale(float time) const {
        if (scaleKeys.empty()) return Vec3{1, 1, 1};
        if (scaleKeys.size() == 1 || time <= scaleKeys.front().time)
            return scaleKeys.front().value;
        if (time >= scaleKeys.back().time)
            return scaleKeys.back().value;

        for (size_t i = 0; i + 1 < scaleKeys.size(); ++i) {
            if (time >= scaleKeys[i].time && time <= scaleKeys[i + 1].time) {
                float span = scaleKeys[i + 1].time - scaleKeys[i].time;
                float t = (span > 0.0f) ? (time - scaleKeys[i].time) / span : 0.0f;
                return Lerp(scaleKeys[i].value, scaleKeys[i + 1].value, t);
            }
        }
        return scaleKeys.back().value;
    }
};

struct AnimationClip {
    std::string name;
    float duration = 0.0f;
    float ticksPerSecond = 24.0f;
    std::vector<BoneAnimation> channels;
    bool loop = true;

    const BoneAnimation* FindChannel(const std::string& boneName) const {
        for (const auto& ch : channels)
            if (ch.boneName == boneName) return &ch;
        return nullptr;
    }
};

// ============================================================================
// Animation Blending
// ============================================================================

enum class BlendMode {
    Override,
    Additive,
    Layered
};

struct AnimationLayer {
    std::string clipName;
    float weight = 1.0f;
    float currentTime = 0.0f;
    float speed = 1.0f;
    BlendMode blendMode = BlendMode::Override;
    bool playing = true;
    bool loop = true;
    std::vector<int32_t> boneMask;
};

// ============================================================================
// Animation State Machine
// ============================================================================

struct AnimationTransition {
    std::string fromState;
    std::string toState;
    float duration = 0.2f;
    std::function<bool()> condition;
    bool hasExitTime = false;
    float exitTime = 1.0f;
};

struct AnimationState {
    std::string name;
    std::string clipName;
    float speed = 1.0f;
    bool loop = true;
};

class AnimationStateMachine {
public:
    void AddState(const AnimationState& state) {
        m_states[state.name] = state;
    }

    void AddTransition(const AnimationTransition& transition) {
        m_transitions.push_back(transition);
    }

    void SetDefaultState(const std::string& stateName) {
        m_defaultState = stateName;
        if (m_currentState.empty())
            m_currentState = stateName;
    }

    void Update(float deltaTime) {
        if (m_currentState.empty() && !m_defaultState.empty())
            m_currentState = m_defaultState;

        if (m_isTransitioning) {
            m_transitionElapsed += deltaTime;
            m_blendFactor = (m_transitionDuration > 0.0f)
                ? std::min(m_transitionElapsed / m_transitionDuration, 1.0f)
                : 1.0f;
            if (m_blendFactor >= 1.0f) {
                m_currentState = m_targetState;
                m_targetState.clear();
                m_isTransitioning = false;
                m_blendFactor = 0.0f;
                m_transitionElapsed = 0.0f;
            }
            return;
        }

        // Evaluate transitions from current state
        for (const auto& t : m_transitions) {
            if (t.fromState == m_currentState && t.condition && t.condition()) {
                m_targetState = t.toState;
                m_transitionDuration = t.duration;
                m_transitionElapsed = 0.0f;
                m_blendFactor = 0.0f;
                m_isTransitioning = true;
                break;
            }
        }

        m_currentTime += deltaTime;
    }

    const std::string& GetCurrentStateName() const { return m_currentState; }
    float GetCurrentTime() const { return m_currentTime; }
    float GetBlendFactor() const { return m_blendFactor; }
    bool IsTransitioning() const { return m_isTransitioning; }
    const std::string& GetTargetStateName() const { return m_targetState; }

    void ForceState(const std::string& stateName) {
        m_currentState = stateName;
        m_isTransitioning = false;
        m_targetState.clear();
        m_blendFactor = 0.0f;
        m_transitionElapsed = 0.0f;
        m_currentTime = 0.0f;
    }

private:
    std::unordered_map<std::string, AnimationState> m_states;
    std::vector<AnimationTransition> m_transitions;
    std::string m_currentState;
    std::string m_targetState;
    std::string m_defaultState;

    float m_currentTime = 0.0f;
    float m_blendFactor = 0.0f;
    float m_transitionDuration = 0.0f;
    float m_transitionElapsed = 0.0f;
    bool m_isTransitioning = false;
};

// ============================================================================
// AnimationManager
// ============================================================================

class AnimationManager {
public:
    void RegisterClip(const std::string& name, std::shared_ptr<AnimationClip> clip) {
        m_clips[name] = std::move(clip);
    }

    std::shared_ptr<AnimationClip> GetClip(const std::string& name) const {
        auto it = m_clips.find(name);
        return (it != m_clips.end()) ? it->second : nullptr;
    }

    void Clear() { m_clips.clear(); }

private:
    std::unordered_map<std::string, std::shared_ptr<AnimationClip>> m_clips;
};

// ============================================================================
// AnimationEvaluator
// ============================================================================

class AnimationEvaluator {
public:
    /// Sample a clip at a given time, producing per-bone local transform matrices
    static void SampleClip(const AnimationClip& clip, const Skeleton& skeleton,
                           float time, std::vector<Mat4x4>& outLocalTransforms) {
        outLocalTransforms.resize(skeleton.GetBoneCount(), Mat4x4::Identity());

        for (const auto& channel : clip.channels) {
            int32_t idx = skeleton.FindBone(channel.boneName);
            if (idx < 0) continue;

            Vec3 pos = channel.InterpolatePosition(time);
            // Encode position into translation part of an identity matrix
            Mat4x4 transform = Mat4x4::Identity();
            transform.m[3][0] = pos.x;
            transform.m[3][1] = pos.y;
            transform.m[3][2] = pos.z;

            // Encode scale on diagonal
            Vec3 scl = channel.InterpolateScale(time);
            transform.m[0][0] = scl.x;
            transform.m[1][1] = scl.y;
            transform.m[2][2] = scl.z;

            outLocalTransforms[idx] = transform;
        }
    }

    /// Blend two sets of local transforms by a factor (0 = all A, 1 = all B)
    static void BlendTransforms(const std::vector<Mat4x4>& a,
                                const std::vector<Mat4x4>& b,
                                float blendFactor,
                                std::vector<Mat4x4>& outResult) {
        size_t count = std::min(a.size(), b.size());
        outResult.resize(count);
        for (size_t i = 0; i < count; ++i) {
            outResult[i] = LerpMat4x4(a[i], b[i], blendFactor);
        }
    }
};

// ============================================================================
// Helper to build a simple test skeleton
// ============================================================================

Skeleton MakeTestSkeleton() {
    Skeleton skel;
    skel.name = "TestSkeleton";
    skel.AddBone("Root", -1);         // 0
    skel.AddBone("Spine", 0);         // 1
    skel.AddBone("Head", 1);          // 2
    skel.AddBone("LeftArm", 1);       // 3
    skel.AddBone("RightArm", 1);      // 4
    skel.AddBone("LeftLeg", 0);       // 5
    skel.AddBone("RightLeg", 0);      // 6
    return skel;
}

AnimationClip MakeTestClip() {
    AnimationClip clip;
    clip.name = "Walk";
    clip.duration = 1.0f;
    clip.ticksPerSecond = 24.0f;
    clip.loop = true;

    BoneAnimation rootChannel;
    rootChannel.boneName = "Root";
    rootChannel.boneIndex = 0;
    rootChannel.positionKeys = {
        {0.0f, {0, 0, 0}},
        {0.5f, {0, 0.1f, 0.5f}},
        {1.0f, {0, 0, 1.0f}}
    };
    rootChannel.rotationKeys = {
        {0.0f, {0, 0, 0, 1}},
        {1.0f, {0, 0, 0, 1}}
    };
    rootChannel.scaleKeys = {
        {0.0f, {1, 1, 1}},
        {1.0f, {1, 1, 1}}
    };
    clip.channels.push_back(rootChannel);

    BoneAnimation spineChannel;
    spineChannel.boneName = "Spine";
    spineChannel.boneIndex = 1;
    spineChannel.positionKeys = {
        {0.0f, {0, 1, 0}},
        {1.0f, {0, 1, 0}}
    };
    spineChannel.scaleKeys = {
        {0.0f, {1, 1, 1}},
        {1.0f, {1, 1, 1}}
    };
    clip.channels.push_back(spineChannel);

    return clip;
}

} // namespace TestAnim

// ============================================================================
// Tests: Skeleton & Bone Hierarchy
// ============================================================================

TEST(Skeleton_BoneHierarchyCreation)
{
    TestAnim::Skeleton skel = TestAnim::MakeTestSkeleton();

    EXPECT_EQ(skel.GetBoneCount(), 7u);
    EXPECT_EQ(skel.bones[0].name, std::string("Root"));
    EXPECT_EQ(skel.bones[0].parentIndex, -1);
    EXPECT_EQ(skel.bones[1].name, std::string("Spine"));
    EXPECT_EQ(skel.bones[1].parentIndex, 0);
    EXPECT_EQ(skel.bones[2].name, std::string("Head"));
    EXPECT_EQ(skel.bones[2].parentIndex, 1);
    EXPECT_EQ(skel.bones[3].parentIndex, 1);  // LeftArm -> Spine
    EXPECT_EQ(skel.bones[4].parentIndex, 1);  // RightArm -> Spine
    EXPECT_EQ(skel.bones[5].parentIndex, 0);  // LeftLeg -> Root
    EXPECT_EQ(skel.bones[6].parentIndex, 0);  // RightLeg -> Root
}

TEST(Skeleton_FindBone)
{
    TestAnim::Skeleton skel = TestAnim::MakeTestSkeleton();

    EXPECT_EQ(skel.FindBone("Root"), 0);
    EXPECT_EQ(skel.FindBone("Spine"), 1);
    EXPECT_EQ(skel.FindBone("Head"), 2);
    EXPECT_EQ(skel.FindBone("LeftArm"), 3);
    EXPECT_EQ(skel.FindBone("RightArm"), 4);
    EXPECT_EQ(skel.FindBone("LeftLeg"), 5);
    EXPECT_EQ(skel.FindBone("RightLeg"), 6);
    EXPECT_EQ(skel.FindBone("NonExistentBone"), -1);
}

// ============================================================================
// Tests: AnimationClip
// ============================================================================

TEST(AnimationClip_CreationAndProperties)
{
    TestAnim::AnimationClip clip = TestAnim::MakeTestClip();

    EXPECT_EQ(clip.name, std::string("Walk"));
    EXPECT_NEAR(clip.duration, 1.0f, 0.001f);
    EXPECT_NEAR(clip.ticksPerSecond, 24.0f, 0.001f);
    EXPECT_TRUE(clip.loop);
    EXPECT_EQ(clip.channels.size(), 2u);
}

TEST(AnimationClip_FindChannel)
{
    TestAnim::AnimationClip clip = TestAnim::MakeTestClip();

    const TestAnim::BoneAnimation* rootCh = clip.FindChannel("Root");
    EXPECT_TRUE(rootCh != nullptr);
    EXPECT_EQ(rootCh->boneName, std::string("Root"));

    const TestAnim::BoneAnimation* spineCh = clip.FindChannel("Spine");
    EXPECT_TRUE(spineCh != nullptr);
    EXPECT_EQ(spineCh->boneName, std::string("Spine"));

    const TestAnim::BoneAnimation* missing = clip.FindChannel("NonExistent");
    EXPECT_TRUE(missing == nullptr);
}

// ============================================================================
// Tests: BoneAnimation keyframe interpolation
// ============================================================================

TEST(BoneAnimation_InterpolatePosition)
{
    TestAnim::AnimationClip clip = TestAnim::MakeTestClip();
    const TestAnim::BoneAnimation* root = clip.FindChannel("Root");

    // At time 0.0 -> first key
    TestAnim::Vec3 pos0 = root->InterpolatePosition(0.0f);
    EXPECT_NEAR(pos0.x, 0.0f, 0.001f);
    EXPECT_NEAR(pos0.y, 0.0f, 0.001f);
    EXPECT_NEAR(pos0.z, 0.0f, 0.001f);

    // At time 0.5 -> second key
    TestAnim::Vec3 pos05 = root->InterpolatePosition(0.5f);
    EXPECT_NEAR(pos05.x, 0.0f, 0.001f);
    EXPECT_NEAR(pos05.y, 0.1f, 0.001f);
    EXPECT_NEAR(pos05.z, 0.5f, 0.001f);

    // At time 0.25 -> between first and second keys (linear interpolation)
    TestAnim::Vec3 pos025 = root->InterpolatePosition(0.25f);
    EXPECT_NEAR(pos025.x, 0.0f, 0.001f);
    EXPECT_NEAR(pos025.y, 0.05f, 0.001f);
    EXPECT_NEAR(pos025.z, 0.25f, 0.001f);

    // At time 1.0 -> last key
    TestAnim::Vec3 pos1 = root->InterpolatePosition(1.0f);
    EXPECT_NEAR(pos1.x, 0.0f, 0.001f);
    EXPECT_NEAR(pos1.y, 0.0f, 0.001f);
    EXPECT_NEAR(pos1.z, 1.0f, 0.001f);
}

TEST(BoneAnimation_InterpolateRotation)
{
    TestAnim::BoneAnimation anim;
    anim.boneName = "Test";
    anim.rotationKeys = {
        {0.0f, {0.0f, 0.0f, 0.0f, 1.0f}},   // Identity quaternion
        {1.0f, {0.0f, 0.7071f, 0.0f, 0.7071f}} // ~90 degrees around Y
    };

    // At time 0 -> first key
    TestAnim::Quat q0 = anim.InterpolateRotation(0.0f);
    EXPECT_NEAR(q0.x, 0.0f, 0.001f);
    EXPECT_NEAR(q0.y, 0.0f, 0.001f);
    EXPECT_NEAR(q0.z, 0.0f, 0.001f);
    EXPECT_NEAR(q0.w, 1.0f, 0.001f);

    // At time 1 -> last key
    TestAnim::Quat q1 = anim.InterpolateRotation(1.0f);
    EXPECT_NEAR(q1.y, 0.7071f, 0.01f);
    EXPECT_NEAR(q1.w, 0.7071f, 0.01f);

    // At midpoint (t=0.5) -> interpolated, should still be a unit quaternion
    TestAnim::Quat qMid = anim.InterpolateRotation(0.5f);
    float qLen = qMid.Length();
    EXPECT_NEAR(qLen, 1.0f, 0.01f);
}

TEST(BoneAnimation_InterpolateScale)
{
    TestAnim::BoneAnimation anim;
    anim.boneName = "Test";
    anim.scaleKeys = {
        {0.0f, {1.0f, 1.0f, 1.0f}},
        {1.0f, {2.0f, 2.0f, 2.0f}}
    };

    // At time 0
    TestAnim::Vec3 s0 = anim.InterpolateScale(0.0f);
    EXPECT_NEAR(s0.x, 1.0f, 0.001f);
    EXPECT_NEAR(s0.y, 1.0f, 0.001f);

    // At time 0.5 -> halfway between
    TestAnim::Vec3 s05 = anim.InterpolateScale(0.5f);
    EXPECT_NEAR(s05.x, 1.5f, 0.001f);
    EXPECT_NEAR(s05.y, 1.5f, 0.001f);
    EXPECT_NEAR(s05.z, 1.5f, 0.001f);

    // At time 1
    TestAnim::Vec3 s1 = anim.InterpolateScale(1.0f);
    EXPECT_NEAR(s1.x, 2.0f, 0.001f);
    EXPECT_NEAR(s1.y, 2.0f, 0.001f);
    EXPECT_NEAR(s1.z, 2.0f, 0.001f);
}

// ============================================================================
// Tests: AnimationStateMachine
// ============================================================================

TEST(AnimationStateMachine_StateTransitions)
{
    TestAnim::AnimationStateMachine sm;

    TestAnim::AnimationState idle;
    idle.name = "Idle";
    idle.clipName = "IdleClip";
    sm.AddState(idle);

    TestAnim::AnimationState walk;
    walk.name = "Walk";
    walk.clipName = "WalkClip";
    sm.AddState(walk);

    TestAnim::AnimationState run;
    run.name = "Run";
    run.clipName = "RunClip";
    sm.AddState(run);

    sm.SetDefaultState("Idle");
    EXPECT_EQ(sm.GetCurrentStateName(), std::string("Idle"));

    // Add transition: Idle -> Walk (always triggers)
    bool shouldWalk = false;
    TestAnim::AnimationTransition t1;
    t1.fromState = "Idle";
    t1.toState = "Walk";
    t1.duration = 0.3f;
    t1.condition = [&]() { return shouldWalk; };
    sm.AddTransition(t1);

    // Update without triggering
    sm.Update(0.016f);
    EXPECT_EQ(sm.GetCurrentStateName(), std::string("Idle"));
    EXPECT_FALSE(sm.IsTransitioning());

    // Trigger transition
    shouldWalk = true;
    sm.Update(0.016f);
    EXPECT_TRUE(sm.IsTransitioning());
    EXPECT_EQ(sm.GetTargetStateName(), std::string("Walk"));

    // Complete the transition (update enough time)
    sm.Update(0.5f);
    EXPECT_FALSE(sm.IsTransitioning());
    EXPECT_EQ(sm.GetCurrentStateName(), std::string("Walk"));
}

TEST(AnimationStateMachine_CrossfadeBlending)
{
    TestAnim::AnimationStateMachine sm;

    TestAnim::AnimationState idle;
    idle.name = "Idle";
    idle.clipName = "IdleClip";
    sm.AddState(idle);

    TestAnim::AnimationState attack;
    attack.name = "Attack";
    attack.clipName = "AttackClip";
    sm.AddState(attack);

    sm.SetDefaultState("Idle");

    bool shouldAttack = true;
    TestAnim::AnimationTransition t;
    t.fromState = "Idle";
    t.toState = "Attack";
    t.duration = 0.4f;
    t.condition = [&]() { return shouldAttack; };
    sm.AddTransition(t);

    // Trigger transition
    sm.Update(0.016f);
    EXPECT_TRUE(sm.IsTransitioning());

    // At 0.2s into a 0.4s transition, blend factor should be ~0.5
    sm.Update(0.2f);
    EXPECT_TRUE(sm.IsTransitioning());
    float bf = sm.GetBlendFactor();
    // Elapsed = 0.016 + 0.2 = 0.216; blendFactor = 0.216/0.4 = 0.54
    EXPECT_GE(bf, 0.0f);
    EXPECT_LE(bf, 1.0f);
    EXPECT_TRUE(bf > 0.3f && bf < 0.8f);  // Roughly mid-transition

    // Complete transition
    sm.Update(0.5f);
    EXPECT_FALSE(sm.IsTransitioning());
    EXPECT_EQ(sm.GetCurrentStateName(), std::string("Attack"));
}

TEST(AnimationStateMachine_ForceState)
{
    TestAnim::AnimationStateMachine sm;

    TestAnim::AnimationState idle;
    idle.name = "Idle";
    sm.AddState(idle);

    TestAnim::AnimationState dead;
    dead.name = "Dead";
    sm.AddState(dead);

    sm.SetDefaultState("Idle");
    EXPECT_EQ(sm.GetCurrentStateName(), std::string("Idle"));

    sm.ForceState("Dead");
    EXPECT_EQ(sm.GetCurrentStateName(), std::string("Dead"));
    EXPECT_FALSE(sm.IsTransitioning());
}

// ============================================================================
// Tests: AnimationLayer
// ============================================================================

TEST(AnimationLayer_Configuration)
{
    TestAnim::AnimationLayer layer;
    layer.clipName = "UpperBodyAttack";
    layer.weight = 0.75f;
    layer.speed = 1.5f;
    layer.blendMode = TestAnim::BlendMode::Layered;
    layer.boneMask = {1, 2, 3, 4};  // Spine, Head, LeftArm, RightArm

    EXPECT_EQ(layer.clipName, std::string("UpperBodyAttack"));
    EXPECT_NEAR(layer.weight, 0.75f, 0.001f);
    EXPECT_NEAR(layer.speed, 1.5f, 0.001f);
    EXPECT_TRUE(layer.blendMode == TestAnim::BlendMode::Layered);
    EXPECT_EQ(layer.boneMask.size(), 4u);
    EXPECT_TRUE(layer.playing);
    EXPECT_TRUE(layer.loop);
}

TEST(AnimationLayer_DefaultValues)
{
    TestAnim::AnimationLayer layer;

    EXPECT_NEAR(layer.weight, 1.0f, 0.001f);
    EXPECT_NEAR(layer.currentTime, 0.0f, 0.001f);
    EXPECT_NEAR(layer.speed, 1.0f, 0.001f);
    EXPECT_TRUE(layer.blendMode == TestAnim::BlendMode::Override);
    EXPECT_TRUE(layer.playing);
    EXPECT_TRUE(layer.loop);
    EXPECT_TRUE(layer.boneMask.empty());
}

// ============================================================================
// Tests: AnimationManager
// ============================================================================

TEST(AnimationManager_RegisterAndRetrieveClip)
{
    TestAnim::AnimationManager mgr;

    auto clip = std::make_shared<TestAnim::AnimationClip>();
    clip->name = "Run";
    clip->duration = 0.8f;

    mgr.RegisterClip("Run", clip);

    auto retrieved = mgr.GetClip("Run");
    EXPECT_TRUE(retrieved != nullptr);
    EXPECT_EQ(retrieved->name, std::string("Run"));
    EXPECT_NEAR(retrieved->duration, 0.8f, 0.001f);
}

TEST(AnimationManager_GetMissingClipReturnsNull)
{
    TestAnim::AnimationManager mgr;
    auto result = mgr.GetClip("DoesNotExist");
    EXPECT_TRUE(result == nullptr);
}

TEST(AnimationManager_ClearRemovesAll)
{
    TestAnim::AnimationManager mgr;
    mgr.RegisterClip("Clip1", std::make_shared<TestAnim::AnimationClip>());
    mgr.RegisterClip("Clip2", std::make_shared<TestAnim::AnimationClip>());

    mgr.Clear();

    EXPECT_TRUE(mgr.GetClip("Clip1") == nullptr);
    EXPECT_TRUE(mgr.GetClip("Clip2") == nullptr);
}

// ============================================================================
// Tests: AnimationEvaluator
// ============================================================================

TEST(AnimationEvaluator_SampleClip)
{
    TestAnim::Skeleton skel = TestAnim::MakeTestSkeleton();
    TestAnim::AnimationClip clip = TestAnim::MakeTestClip();

    std::vector<TestAnim::Mat4x4> localTransforms;
    TestAnim::AnimationEvaluator::SampleClip(clip, skel, 0.5f, localTransforms);

    EXPECT_EQ(localTransforms.size(), skel.GetBoneCount());

    // Root bone at time 0.5: position should be (0, 0.1, 0.5), scale (1,1,1)
    EXPECT_NEAR(localTransforms[0].m[3][0], 0.0f, 0.001f);   // x translation
    EXPECT_NEAR(localTransforms[0].m[3][1], 0.1f, 0.001f);   // y translation
    EXPECT_NEAR(localTransforms[0].m[3][2], 0.5f, 0.001f);   // z translation
    EXPECT_NEAR(localTransforms[0].m[0][0], 1.0f, 0.001f);   // x scale
    EXPECT_NEAR(localTransforms[0].m[1][1], 1.0f, 0.001f);   // y scale
    EXPECT_NEAR(localTransforms[0].m[2][2], 1.0f, 0.001f);   // z scale
}

TEST(AnimationEvaluator_BlendTransforms)
{
    size_t boneCount = 3;

    std::vector<TestAnim::Mat4x4> setA(boneCount, TestAnim::Mat4x4::Identity());
    std::vector<TestAnim::Mat4x4> setB(boneCount, TestAnim::Mat4x4::Identity());

    // Set A: root at origin
    setA[0].m[3][0] = 0.0f;
    setA[0].m[3][1] = 0.0f;
    setA[0].m[3][2] = 0.0f;

    // Set B: root at (10, 0, 0)
    setB[0].m[3][0] = 10.0f;
    setB[0].m[3][1] = 0.0f;
    setB[0].m[3][2] = 0.0f;

    // Blend at factor 0 -> should be A
    std::vector<TestAnim::Mat4x4> result0;
    TestAnim::AnimationEvaluator::BlendTransforms(setA, setB, 0.0f, result0);
    EXPECT_NEAR(result0[0].m[3][0], 0.0f, 0.001f);

    // Blend at factor 0.5 -> should be midpoint
    std::vector<TestAnim::Mat4x4> result05;
    TestAnim::AnimationEvaluator::BlendTransforms(setA, setB, 0.5f, result05);
    EXPECT_NEAR(result05[0].m[3][0], 5.0f, 0.001f);

    // Blend at factor 1 -> should be B
    std::vector<TestAnim::Mat4x4> result1;
    TestAnim::AnimationEvaluator::BlendTransforms(setA, setB, 1.0f, result1);
    EXPECT_NEAR(result1[0].m[3][0], 10.0f, 0.001f);
}
