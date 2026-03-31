/**
 * @file TestPerceptionSystemMath.cpp
 * @brief Tests for PerceptionSystem: FOV cone math, hearing radius,
 *        memory decay, confidence tracking, and spatial queries.
 *
 * Standalone reimplementation — mirrors Spark::AI::Perception functions
 * without engine/DirectXMath dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{

    constexpr float PI = 3.14159265358979323846f;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        float LengthSquared() const { return x * x + y * y + z * z; }

        Vec3 Normalized() const
        {
            float len = Length();
            if (len < 0.0001f)
                return {0, 0, 0};
            return {x / len, y / len, z / len};
        }

        float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }

        float DistanceTo(const Vec3& o) const { return (*this - o).Length(); }
    };

    using PerceptionEntityID = uint32_t;

    // --- PerceptionMemory ---

    struct PerceptionMemory
    {
        Vec3 lastSeenPosition;
        float lastSeenTime = 0.0f;
        float confidence = 0.0f;

        void Update(const Vec3& position, float currentTime, float newConfidence)
        {
            lastSeenPosition = position;
            lastSeenTime = currentTime;
            confidence = std::min(1.0f, newConfidence);
        }

        void Decay(float currentTime, float decayRate)
        {
            float elapsed = currentTime - lastSeenTime;
            confidence = std::max(0.0f, confidence - elapsed * decayRate);
        }

        bool IsValid(float minConfidence) const { return confidence >= minConfidence; }
    };

    // --- PerceptionComponent ---

    struct PerceptionComponent
    {
        float sightRange = 50.0f;
        float sightFOV = 90.0f; // degrees, total angle
        float hearingRange = 30.0f;
        float memoryDecayRate = 0.1f; // confidence per second
        float minConfidence = 0.05f;

        std::unordered_map<PerceptionEntityID, PerceptionMemory> memories;

        void Remember(PerceptionEntityID id, const Vec3& position, float currentTime, float confidence)
        {
            memories[id].Update(position, currentTime, confidence);
        }

        void DecayAndPrune(float currentTime)
        {
            std::vector<PerceptionEntityID> toRemove;
            for (auto& [id, mem] : memories)
            {
                mem.Decay(currentTime, memoryDecayRate);
                if (!mem.IsValid(minConfidence))
                    toRemove.push_back(id);
            }
            for (auto id : toRemove)
                memories.erase(id);
        }

        const PerceptionMemory* GetMemory(PerceptionEntityID id) const
        {
            auto it = memories.find(id);
            return (it != memories.end()) ? &it->second : nullptr;
        }

        PerceptionEntityID GetMostConfidentTarget() const
        {
            PerceptionEntityID best = 0;
            float bestConf = 0.0f;
            for (auto& [id, mem] : memories)
            {
                if (mem.confidence > bestConf)
                {
                    bestConf = mem.confidence;
                    best = id;
                }
            }
            return best;
        }

        void ClearMemories() { memories.clear(); }
    };

    // --- Perception functions (mirror Spark::AI::Perception) ---

    bool CanSee(const Vec3& observerPos, const Vec3& observerForward, const Vec3& targetPos, float fovDegrees,
                float maxRange)
    {
        Vec3 toTarget = targetPos - observerPos;
        float distance = toTarget.Length();

        if (distance > maxRange)
            return false;
        if (distance < 0.0001f)
            return true; // on top of observer

        Vec3 dir = toTarget.Normalized();
        float dot = observerForward.Normalized().Dot(dir);

        // FOV is total angle, so half-angle check
        float halfFOVRad = (fovDegrees * 0.5f) * (PI / 180.0f);
        float cosHalfFOV = std::cos(halfFOVRad);

        return dot >= cosHalfFOV;
    }

    bool CanHear(const Vec3& listenerPos, const Vec3& soundPos, float soundRadius)
    {
        float distance = listenerPos.DistanceTo(soundPos);
        return distance <= soundRadius;
    }

    bool CanHearWithLoudness(const Vec3& listenerPos, const Vec3& soundPos, float soundRadius, float& loudness)
    {
        float distance = listenerPos.DistanceTo(soundPos);
        if (distance > soundRadius)
        {
            loudness = 0.0f;
            return false;
        }
        // Linear falloff
        loudness = 1.0f - (distance / soundRadius);
        return true;
    }

} // anonymous namespace

// ============================================================================
// FOV Cone Math Tests
// ============================================================================

TEST(Perception_CanSee_DirectlyAhead)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);
    Vec3 target(0, 0, 10);

    EXPECT_TRUE(CanSee(observer, forward, target, 90.0f, 50.0f));
}

TEST(Perception_CanSee_Behind)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);
    Vec3 target(0, 0, -10); // behind

    EXPECT_FALSE(CanSee(observer, forward, target, 90.0f, 50.0f));
}

TEST(Perception_CanSee_OnEdgeOfFOV)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);

    // 90 degree FOV = 45 degree half-angle
    // At exactly 44 degrees should be visible
    float angle = 44.0f * PI / 180.0f;
    Vec3 target(std::sin(angle) * 10.0f, 0, std::cos(angle) * 10.0f);

    EXPECT_TRUE(CanSee(observer, forward, target, 90.0f, 50.0f));
}

TEST(Perception_CanSee_OutsideFOV)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);

    // 90 degree FOV = 45 degree half-angle
    // At 50 degrees should NOT be visible
    float angle = 50.0f * PI / 180.0f;
    Vec3 target(std::sin(angle) * 10.0f, 0, std::cos(angle) * 10.0f);

    EXPECT_FALSE(CanSee(observer, forward, target, 90.0f, 50.0f));
}

TEST(Perception_CanSee_OutOfRange)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);
    Vec3 target(0, 0, 100); // beyond 50m range

    EXPECT_FALSE(CanSee(observer, forward, target, 90.0f, 50.0f));
}

TEST(Perception_CanSee_ExactlyAtRange)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);
    Vec3 target(0, 0, 50.0f); // exactly at range

    EXPECT_TRUE(CanSee(observer, forward, target, 90.0f, 50.0f));
}

TEST(Perception_CanSee_SamePosition)
{
    Vec3 observer(5, 5, 5);
    Vec3 forward(0, 0, 1);
    Vec3 target(5, 5, 5); // same position

    EXPECT_TRUE(CanSee(observer, forward, target, 90.0f, 50.0f));
}

TEST(Perception_CanSee_WideFOV)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);

    // 180 degree FOV — should see everything in front hemisphere
    float angle = 85.0f * PI / 180.0f;
    Vec3 target(std::sin(angle) * 10.0f, 0, std::cos(angle) * 10.0f);

    EXPECT_TRUE(CanSee(observer, forward, target, 180.0f, 50.0f));
}

TEST(Perception_CanSee_NarrowFOV)
{
    Vec3 observer(0, 0, 0);
    Vec3 forward(0, 0, 1);

    // 10 degree FOV — very narrow
    float angle = 10.0f * PI / 180.0f;
    Vec3 target(std::sin(angle) * 10.0f, 0, std::cos(angle) * 10.0f);

    EXPECT_FALSE(CanSee(observer, forward, target, 10.0f, 50.0f));
}

// ============================================================================
// Hearing Radius Tests
// ============================================================================

TEST(Perception_CanHear_InRange)
{
    Vec3 listener(0, 0, 0);
    Vec3 sound(10, 0, 0);

    EXPECT_TRUE(CanHear(listener, sound, 15.0f));
}

TEST(Perception_CanHear_OutOfRange)
{
    Vec3 listener(0, 0, 0);
    Vec3 sound(20, 0, 0);

    EXPECT_FALSE(CanHear(listener, sound, 15.0f));
}

TEST(Perception_CanHear_ExactlyAtRange)
{
    Vec3 listener(0, 0, 0);
    Vec3 sound(15, 0, 0);

    EXPECT_TRUE(CanHear(listener, sound, 15.0f));
}

TEST(Perception_LoudnessLinearFalloff)
{
    Vec3 listener(0, 0, 0);
    float loudness = 0.0f;

    // At distance 0 — full loudness
    EXPECT_TRUE(CanHearWithLoudness(listener, Vec3(0, 0, 0), 10.0f, loudness));
    EXPECT_NEAR(loudness, 1.0f, 0.01f);

    // At half distance — half loudness
    EXPECT_TRUE(CanHearWithLoudness(listener, Vec3(5, 0, 0), 10.0f, loudness));
    EXPECT_NEAR(loudness, 0.5f, 0.01f);

    // At full distance — zero loudness
    EXPECT_TRUE(CanHearWithLoudness(listener, Vec3(10, 0, 0), 10.0f, loudness));
    EXPECT_NEAR(loudness, 0.0f, 0.01f);
}

TEST(Perception_LoudnessOutOfRange)
{
    Vec3 listener(0, 0, 0);
    float loudness = 1.0f;

    EXPECT_FALSE(CanHearWithLoudness(listener, Vec3(20, 0, 0), 10.0f, loudness));
    EXPECT_NEAR(loudness, 0.0f, 0.01f);
}

// ============================================================================
// Memory Decay Tests
// ============================================================================

TEST(Perception_MemoryUpdate)
{
    PerceptionMemory mem;
    mem.Update(Vec3(10, 0, 5), 1.0f, 0.8f);

    EXPECT_NEAR(mem.lastSeenPosition.x, 10.0f, 0.01f);
    EXPECT_NEAR(mem.lastSeenTime, 1.0f, 0.01f);
    EXPECT_NEAR(mem.confidence, 0.8f, 0.01f);
}

TEST(Perception_MemoryDecay)
{
    PerceptionMemory mem;
    mem.Update(Vec3(10, 0, 5), 0.0f, 1.0f);

    // After 5 seconds with 0.1/sec decay rate
    mem.Decay(5.0f, 0.1f);
    EXPECT_NEAR(mem.confidence, 0.5f, 0.01f);
}

TEST(Perception_MemoryDecayToZero)
{
    PerceptionMemory mem;
    mem.Update(Vec3(0, 0, 0), 0.0f, 0.5f);

    // After 10 seconds with 0.1/sec decay
    mem.Decay(10.0f, 0.1f);
    EXPECT_NEAR(mem.confidence, 0.0f, 0.01f); // clamped at 0
}

TEST(Perception_MemoryValidityCheck)
{
    PerceptionMemory mem;
    mem.confidence = 0.3f;

    EXPECT_TRUE(mem.IsValid(0.1f));
    EXPECT_TRUE(mem.IsValid(0.3f));
    EXPECT_FALSE(mem.IsValid(0.5f));
}

TEST(Perception_ConfidenceClampedAtOne)
{
    PerceptionMemory mem;
    mem.Update(Vec3(0, 0, 0), 0.0f, 5.0f); // over 1.0
    EXPECT_NEAR(mem.confidence, 1.0f, 0.01f);
}

// ============================================================================
// PerceptionComponent Tests
// ============================================================================

TEST(Perception_RememberTarget)
{
    PerceptionComponent comp;
    comp.Remember(42, Vec3(10, 0, 5), 1.0f, 0.9f);

    auto* mem = comp.GetMemory(42);
    EXPECT_TRUE(mem != nullptr);
    EXPECT_NEAR(mem->confidence, 0.9f, 0.01f);
    EXPECT_NEAR(mem->lastSeenPosition.x, 10.0f, 0.01f);
}

TEST(Perception_GetMostConfidentTarget)
{
    PerceptionComponent comp;
    comp.Remember(1, Vec3(10, 0, 0), 1.0f, 0.3f);
    comp.Remember(2, Vec3(20, 0, 0), 1.0f, 0.9f);
    comp.Remember(3, Vec3(30, 0, 0), 1.0f, 0.5f);

    EXPECT_EQ(comp.GetMostConfidentTarget(), static_cast<PerceptionEntityID>(2));
}

TEST(Perception_DecayAndPrune)
{
    PerceptionComponent comp;
    comp.memoryDecayRate = 0.2f;
    comp.minConfidence = 0.1f;

    comp.Remember(1, Vec3(10, 0, 0), 0.0f, 0.5f);
    comp.Remember(2, Vec3(20, 0, 0), 0.0f, 1.0f);

    // After 3 seconds: entity 1 confidence = 0.5 - 0.6 = 0 (pruned)
    //                   entity 2 confidence = 1.0 - 0.6 = 0.4 (kept)
    comp.DecayAndPrune(3.0f);

    EXPECT_TRUE(comp.GetMemory(1) == nullptr);
    EXPECT_TRUE(comp.GetMemory(2) != nullptr);
    EXPECT_NEAR(comp.GetMemory(2)->confidence, 0.4f, 0.01f);
}

TEST(Perception_ClearMemories)
{
    PerceptionComponent comp;
    comp.Remember(1, Vec3(0, 0, 0), 0.0f, 1.0f);
    comp.Remember(2, Vec3(0, 0, 0), 0.0f, 1.0f);

    comp.ClearMemories();
    EXPECT_TRUE(comp.GetMemory(1) == nullptr);
    EXPECT_TRUE(comp.GetMemory(2) == nullptr);
}

TEST(Perception_NoTargetReturnsZero)
{
    PerceptionComponent comp;
    EXPECT_EQ(comp.GetMostConfidentTarget(), static_cast<PerceptionEntityID>(0));
}

TEST(Perception_UpdateExistingMemory)
{
    PerceptionComponent comp;
    comp.Remember(1, Vec3(10, 0, 0), 0.0f, 0.5f);
    comp.Remember(1, Vec3(15, 0, 0), 1.0f, 0.9f); // update

    auto* mem = comp.GetMemory(1);
    EXPECT_NEAR(mem->lastSeenPosition.x, 15.0f, 0.01f);
    EXPECT_NEAR(mem->confidence, 0.9f, 0.01f);
}
