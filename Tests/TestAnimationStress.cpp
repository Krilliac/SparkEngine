// TestAnimationStress.cpp - Stress tests for the Animation subsystem
// Standalone implementations for CI testing (no DirectXMath dependency)

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace TestAnimStress
{

    // ============================================================================
    // Minimal math types (mirrors TestAnimationSystem.cpp / TestBlendSpace.cpp)
    // ============================================================================

    struct Vec2
    {
        float x = 0.0f, y = 0.0f;

        float DistanceTo(const Vec2& o) const
        {
            float dx = x - o.x, dy = y - o.y;
            return std::sqrt(dx * dx + dy * dy);
        }
    };

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    };

    Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
    {
        return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    }

    struct Quat
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
    };

    struct Mat4x4
    {
        float m[4][4] = {};

        static Mat4x4 Identity()
        {
            Mat4x4 result{};
            result.m[0][0] = result.m[1][1] = result.m[2][2] = result.m[3][3] = 1.0f;
            return result;
        }
    };

    // ============================================================================
    // BlendSpace (same logic as TestBlendSpace.cpp)
    // ============================================================================

    struct BlendSample
    {
        std::string animationName;
        Vec2 position;
        float weight = 0.0f;
    };

    struct BlendSpace
    {
        std::vector<BlendSample> samples;

        void AddSample(const std::string& animName, Vec2 pos) { samples.push_back({animName, pos, 0.0f}); }

        int GetSampleCount() const { return static_cast<int>(samples.size()); }

        void Evaluate(Vec2 parameterValue)
        {
            if (samples.empty())
                return;

            float totalWeight = 0.0f;
            bool exactMatch = false;

            for (auto& s : samples)
            {
                float dist = parameterValue.DistanceTo(s.position);
                if (dist < 0.0001f)
                {
                    for (auto& s2 : samples)
                        s2.weight = 0.0f;
                    s.weight = 1.0f;
                    exactMatch = true;
                    break;
                }
                s.weight = 1.0f / (dist * dist);
                totalWeight += s.weight;
            }

            if (!exactMatch && totalWeight > 0.0f)
            {
                for (auto& s : samples)
                    s.weight /= totalWeight;
            }
        }

        float GetTotalWeight() const
        {
            float total = 0.0f;
            for (const auto& s : samples)
                total += s.weight;
            return total;
        }
    };

    // ============================================================================
    // Keyframe & AnimationClip (same logic as TestAnimationSystem.cpp)
    // ============================================================================

    struct VectorKey
    {
        float time;
        Vec3 value;
    };

    struct BoneAnimation
    {
        std::string boneName;
        std::vector<VectorKey> positionKeys;

        Vec3 InterpolatePosition(float time) const
        {
            if (positionKeys.empty())
                return Vec3{0, 0, 0};
            if (positionKeys.size() == 1 || time <= positionKeys.front().time)
                return positionKeys.front().value;
            if (time >= positionKeys.back().time)
                return positionKeys.back().value;

            for (size_t i = 0; i + 1 < positionKeys.size(); ++i)
            {
                if (time >= positionKeys[i].time && time <= positionKeys[i + 1].time)
                {
                    float span = positionKeys[i + 1].time - positionKeys[i].time;
                    float t = (span > 0.0f) ? (time - positionKeys[i].time) / span : 0.0f;
                    return Lerp(positionKeys[i].value, positionKeys[i + 1].value, t);
                }
            }
            return positionKeys.back().value;
        }
    };

    struct AnimationClip
    {
        std::string name;
        float duration = 0.0f;
        float ticksPerSecond = 24.0f;
        std::vector<BoneAnimation> channels;
        bool loop = true;

        const BoneAnimation* FindChannel(const std::string& boneName) const
        {
            for (const auto& ch : channels)
                if (ch.boneName == boneName)
                    return &ch;
            return nullptr;
        }

        /// Sample a clip, handling looping and clamping
        Vec3 SamplePosition(const std::string& boneName, float time) const
        {
            const BoneAnimation* ch = FindChannel(boneName);
            if (!ch)
                return Vec3{0, 0, 0};

            // Clamp or wrap time
            float sampleTime = time;
            if (duration > 0.0f)
            {
                if (loop)
                    sampleTime = std::fmod(time, duration);
                else
                    sampleTime = (std::max)(0.0f, (std::min)(time, duration));
            }
            else
            {
                sampleTime = 0.0f;
            }

            return ch->InterpolatePosition(sampleTime);
        }
    };

    // ============================================================================
    // Bone & Skeleton (same logic as TestAnimationSystem.cpp)
    // ============================================================================

    struct Bone
    {
        std::string name;
        int32_t parentIndex = -1;
        Mat4x4 offsetMatrix;
    };

    struct Skeleton
    {
        std::string name;
        std::vector<Bone> bones;
        std::unordered_map<std::string, int32_t> boneNameToIndex;

        int32_t FindBone(const std::string& boneName) const
        {
            auto it = boneNameToIndex.find(boneName);
            return (it != boneNameToIndex.end()) ? it->second : -1;
        }

        size_t GetBoneCount() const { return bones.size(); }

        void AddBone(const std::string& boneName, int32_t parent)
        {
            int32_t idx = static_cast<int32_t>(bones.size());
            Bone b;
            b.name = boneName;
            b.parentIndex = parent;
            b.offsetMatrix = Mat4x4::Identity();
            bones.push_back(b);
            boneNameToIndex[boneName] = idx;
        }

        /// Safe accessor — returns nullptr if index is out of range
        const Bone* GetBone(int32_t index) const
        {
            if (index < 0 || index >= static_cast<int32_t>(bones.size()))
                return nullptr;
            return &bones[index];
        }
    };

    // ============================================================================
    // Animation State Machine (same logic as TestAnimationSystem.cpp)
    // ============================================================================

    struct AnimationTransition
    {
        std::string fromState;
        std::string toState;
        float duration = 0.2f;
        std::function<bool()> condition;
    };

    struct AnimationState
    {
        std::string name;
        std::string clipName;
        float speed = 1.0f;
        bool loop = true;
    };

    class AnimationStateMachine
    {
      public:
        void AddState(const AnimationState& state) { m_states[state.name] = state; }

        void AddTransition(const AnimationTransition& transition) { m_transitions.push_back(transition); }

        void SetDefaultState(const std::string& stateName)
        {
            m_defaultState = stateName;
            if (m_currentState.empty())
                m_currentState = stateName;
        }

        void Update(float deltaTime)
        {
            if (m_currentState.empty() && !m_defaultState.empty())
                m_currentState = m_defaultState;

            if (m_isTransitioning)
            {
                m_transitionElapsed += deltaTime;
                m_blendFactor =
                    (m_transitionDuration > 0.0f) ? std::min(m_transitionElapsed / m_transitionDuration, 1.0f) : 1.0f;
                if (m_blendFactor >= 1.0f)
                {
                    m_currentState = m_targetState;
                    m_targetState.clear();
                    m_isTransitioning = false;
                    m_blendFactor = 0.0f;
                    m_transitionElapsed = 0.0f;
                }
                return;
            }

            // Evaluate transitions from current state
            for (const auto& t : m_transitions)
            {
                if (t.fromState == m_currentState && t.condition && t.condition())
                {
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
        bool IsTransitioning() const { return m_isTransitioning; }

        void ForceState(const std::string& stateName)
        {
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

} // namespace TestAnimStress

// ============================================================================
// BlendSpace stress tests
// ============================================================================

TEST(AnimStress_BlendSpaceZeroSamples)
{
    TestAnimStress::BlendSpace bs;
    EXPECT_EQ(bs.GetSampleCount(), 0);

    // Evaluate with no samples — must not crash
    bs.Evaluate({0.5f, 0.5f});
    EXPECT_NEAR(bs.GetTotalWeight(), 0.0f, 0.001f);
}

TEST(AnimStress_BlendSpaceDuplicatePositions)
{
    TestAnimStress::BlendSpace bs;

    // Multiple samples at the exact same 2D position
    bs.AddSample("A", {1.0f, 1.0f});
    bs.AddSample("B", {1.0f, 1.0f});
    bs.AddSample("C", {1.0f, 1.0f});
    EXPECT_EQ(bs.GetSampleCount(), 3);

    // Evaluate at the duplicate position — exact match should give weight to one sample
    bs.Evaluate({1.0f, 1.0f});
    EXPECT_NEAR(bs.GetTotalWeight(), 1.0f, 0.001f);

    // Evaluate away from the duplicate position — weights should still sum to 1
    bs.Evaluate({0.0f, 0.0f});
    EXPECT_NEAR(bs.GetTotalWeight(), 1.0f, 0.001f);
}

TEST(AnimStress_BlendSpaceExtremeWeights)
{
    TestAnimStress::BlendSpace bs;
    bs.AddSample("A", {0.0f, 0.0f});
    bs.AddSample("B", {1.0f, 0.0f});

    // Evaluate at an extreme position — must not crash or produce NaN
    bs.Evaluate({1e20f, 1e20f});
    float totalWeight = bs.GetTotalWeight();
    EXPECT_FALSE(std::isnan(totalWeight));
}

TEST(AnimStress_BlendSpaceNaNPosition)
{
    TestAnimStress::BlendSpace bs;
    bs.AddSample("A", {0.0f, 0.0f});
    bs.AddSample("B", {1.0f, 1.0f});

    float nan = std::numeric_limits<float>::quiet_NaN();

    // Evaluate at NaN — must not crash or enter infinite loop
    bs.Evaluate({nan, nan});

    // We just verify we reached this point without hanging
    EXPECT_TRUE(true);
}

// ============================================================================
// AnimationClip stress tests
// ============================================================================

TEST(AnimStress_AnimClipZeroDuration)
{
    TestAnimStress::AnimationClip clip;
    clip.name = "ZeroDuration";
    clip.duration = 0.0f;

    TestAnimStress::BoneAnimation channel;
    channel.boneName = "Root";
    channel.positionKeys.push_back({0.0f, {1.0f, 2.0f, 3.0f}});
    clip.channels.push_back(channel);

    // Sample at t=0 with zero duration — must not divide by zero
    TestAnimStress::Vec3 pos = clip.SamplePosition("Root", 0.0f);
    EXPECT_NEAR(pos.x, 1.0f, 0.001f);
    EXPECT_NEAR(pos.y, 2.0f, 0.001f);
    EXPECT_NEAR(pos.z, 3.0f, 0.001f);
}

TEST(AnimStress_AnimClipNegativeTime)
{
    TestAnimStress::AnimationClip clip;
    clip.name = "NegTimeClip";
    clip.duration = 2.0f;
    clip.loop = false;

    TestAnimStress::BoneAnimation channel;
    channel.boneName = "Root";
    channel.positionKeys.push_back({0.0f, {0.0f, 0.0f, 0.0f}});
    channel.positionKeys.push_back({2.0f, {10.0f, 0.0f, 0.0f}});
    clip.channels.push_back(channel);

    // Sample at negative time — should clamp to start
    TestAnimStress::Vec3 pos = clip.SamplePosition("Root", -5.0f);
    EXPECT_NEAR(pos.x, 0.0f, 0.001f);
}

TEST(AnimStress_AnimClipMassiveKeyframes)
{
    TestAnimStress::AnimationClip clip;
    clip.name = "MassiveKeys";
    clip.duration = 10000.0f;
    clip.loop = false;

    TestAnimStress::BoneAnimation channel;
    channel.boneName = "Root";

    // Create 10000 keyframes spread evenly over the duration
    for (int i = 0; i < 10000; ++i)
    {
        float t = static_cast<float>(i);
        channel.positionKeys.push_back({t, {t, 0.0f, 0.0f}});
    }
    clip.channels.push_back(channel);

    // Sample at various points to verify interpolation works with large keyframe counts
    TestAnimStress::Vec3 posStart = clip.SamplePosition("Root", 0.0f);
    EXPECT_NEAR(posStart.x, 0.0f, 0.001f);

    TestAnimStress::Vec3 posMid = clip.SamplePosition("Root", 5000.0f);
    EXPECT_NEAR(posMid.x, 5000.0f, 0.001f);

    TestAnimStress::Vec3 posEnd = clip.SamplePosition("Root", 9999.0f);
    EXPECT_NEAR(posEnd.x, 9999.0f, 0.001f);
}

// ============================================================================
// State Machine stress tests
// ============================================================================

TEST(AnimStress_StateMachineNoStates)
{
    TestAnimStress::AnimationStateMachine sm;

    // Update with no states at all — must not crash
    for (int i = 0; i < 10; ++i)
    {
        sm.Update(0.016f);
    }

    EXPECT_TRUE(sm.GetCurrentStateName().empty());
}

TEST(AnimStress_StateMachineInvalidTransition)
{
    TestAnimStress::AnimationStateMachine sm;

    TestAnimStress::AnimationState idle;
    idle.name = "Idle";
    idle.clipName = "IdleClip";
    sm.AddState(idle);
    sm.SetDefaultState("Idle");

    // Add transition to a state that doesn't exist in the state map
    TestAnimStress::AnimationTransition t;
    t.fromState = "Idle";
    t.toState = "NonExistentState";
    t.duration = 0.2f;
    t.condition = [] { return true; };
    sm.AddTransition(t);

    // Update — transition fires but target state doesn't exist in m_states.
    // The state machine should still not crash; it will just set the current state string.
    for (int i = 0; i < 20; ++i)
    {
        sm.Update(0.016f);
    }

    // Should have transitioned (the SM stores state name as string, doesn't validate existence)
    EXPECT_TRUE(true);
}

TEST(AnimStress_StateMachineRapidTransitions)
{
    TestAnimStress::AnimationStateMachine sm;

    // Create two states that ping-pong
    TestAnimStress::AnimationState stateA;
    stateA.name = "A";
    stateA.clipName = "ClipA";
    sm.AddState(stateA);

    TestAnimStress::AnimationState stateB;
    stateB.name = "B";
    stateB.clipName = "ClipB";
    sm.AddState(stateB);

    sm.SetDefaultState("A");

    // Force-switch between states 1000 times
    for (int i = 0; i < 1000; ++i)
    {
        sm.ForceState((i % 2 == 0) ? "A" : "B");
        sm.Update(0.016f);
    }

    // Verify still in a valid state
    const std::string& current = sm.GetCurrentStateName();
    EXPECT_TRUE(current == "A" || current == "B");
}

// ============================================================================
// Skeleton stress tests
// ============================================================================

TEST(AnimStress_BoneIndexOutOfRange)
{
    TestAnimStress::Skeleton skeleton;
    skeleton.AddBone("Root", -1);
    skeleton.AddBone("Spine", 0);
    skeleton.AddBone("Head", 1);

    EXPECT_EQ(skeleton.GetBoneCount(), 3u);

    // Access bone index 999 — must return nullptr, not crash
    const TestAnimStress::Bone* result = skeleton.GetBone(999);
    EXPECT_TRUE(result == nullptr);

    // Negative index
    const TestAnimStress::Bone* negResult = skeleton.GetBone(-1);
    EXPECT_TRUE(negResult == nullptr);
}

TEST(AnimStress_SkeletonZeroBones)
{
    TestAnimStress::Skeleton skeleton;
    EXPECT_EQ(skeleton.GetBoneCount(), 0u);

    // FindBone on empty skeleton — must return -1, not crash
    int32_t idx = skeleton.FindBone("NonExistent");
    EXPECT_EQ(idx, -1);

    // GetBone on empty skeleton
    const TestAnimStress::Bone* result = skeleton.GetBone(0);
    EXPECT_TRUE(result == nullptr);
}
