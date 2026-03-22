// TestPhysicsInterpolation.cpp - Tests for physics body interpolation
// Standalone implementations for CI testing (no DirectXMath/Jolt dependency)

#include "TestFramework.h"
#include <cmath>
#include <algorithm>

namespace TestPhysicsInterp
{

    // ============================================================================
    // Minimal math types for cross-platform testing
    // ============================================================================

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

        float DistanceTo(const Vec3& o) const
        {
            float dx = x - o.x, dy = y - o.y, dz = z - o.z;
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        }
    };

    Vec3 Lerp(const Vec3& a, const Vec3& b, float t)
    {
        return {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y), a.z + t * (b.z - a.z)};
    }

    // ============================================================================
    // Minimal quaternion for rotation interpolation testing
    // ============================================================================

    struct Quat
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;

        Quat() = default;
        Quat(float x_, float y_, float z_, float w_) : x(x_), y(y_), z(z_), w(w_) {}

        float Dot(const Quat& o) const { return x * o.x + y * o.y + z * o.z + w * o.w; }

        float Length() const { return std::sqrt(x * x + y * y + z * z + w * w); }

        Quat Normalized() const
        {
            float len = Length();
            if (len < 0.0001f)
                return {0, 0, 0, 1};
            return {x / len, y / len, z / len, w / len};
        }
    };

    Quat Slerp(const Quat& a, const Quat& b, float t)
    {
        float dot = a.Dot(b);
        Quat bAdj = b;
        if (dot < 0.0f)
        {
            dot = -dot;
            bAdj = {-b.x, -b.y, -b.z, -b.w};
        }

        if (dot > 0.9995f)
        {
            // Linear interpolation for very close quaternions
            Quat result = {a.x + t * (bAdj.x - a.x), a.y + t * (bAdj.y - a.y), a.z + t * (bAdj.z - a.z),
                           a.w + t * (bAdj.w - a.w)};
            return result.Normalized();
        }

        float theta = std::acos(std::clamp(dot, -1.0f, 1.0f));
        float sinTheta = std::sin(theta);
        float wa = std::sin((1.0f - t) * theta) / sinTheta;
        float wb = std::sin(t * theta) / sinTheta;

        return {wa * a.x + wb * bAdj.x, wa * a.y + wb * bAdj.y, wa * a.z + wb * bAdj.z, wa * a.w + wb * bAdj.w};
    }

    // ============================================================================
    // Simulated PhysicsBody with interpolation state
    // ============================================================================

    class TestPhysicsBody
    {
      public:
        Vec3 m_previousPosition;
        Quat m_previousRotation;
        Vec3 m_currentPosition;
        Quat m_currentRotation;

        TestPhysicsBody() = default;
        TestPhysicsBody(const Vec3& pos, const Quat& rot)
            : m_previousPosition(pos), m_previousRotation(rot), m_currentPosition(pos), m_currentRotation(rot)
        {
        }

        Vec3 GetInterpolatedPosition(float alpha) const { return Lerp(m_previousPosition, m_currentPosition, alpha); }

        void StoreCurrentState()
        {
            m_previousPosition = m_currentPosition;
            m_previousRotation = m_currentRotation;
        }

        void SimulateStep(const Vec3& newPos, const Quat& newRot)
        {
            m_currentPosition = newPos;
            m_currentRotation = newRot;
        }
    };

    // ============================================================================
    // Simulated PhysicsSystem with fixed-timestep accumulator
    // ============================================================================

    class TestPhysicsSystem
    {
      public:
        float m_timeStep = 1.0f / 60.0f;
        float m_accumulator = 0.0f;
        float m_interpolationAlpha = 1.0f;
        bool m_interpolationEnabled = true;

        float GetInterpolationAlpha() const { return m_interpolationAlpha; }

        void Update(float deltaTime, TestPhysicsBody& body, const Vec3& velocity)
        {
            if (m_interpolationEnabled)
            {
                m_accumulator += deltaTime;
                while (m_accumulator >= m_timeStep)
                {
                    body.StoreCurrentState();
                    // Simulate: move body by velocity * timeStep
                    Vec3 newPos = body.m_currentPosition + velocity * m_timeStep;
                    body.SimulateStep(newPos, body.m_currentRotation);
                    m_accumulator -= m_timeStep;
                }
                m_interpolationAlpha = (m_timeStep > 0.0f) ? m_accumulator / m_timeStep : 1.0f;
            }
        }
    };

} // namespace TestPhysicsInterp

// ============================================================================
// Tests
// ============================================================================

TEST(PhysicsInterp_AlphaZero_ReturnsPreviousPosition)
{
    TestPhysicsInterp::TestPhysicsBody body({0, 0, 0}, {});
    body.StoreCurrentState();
    body.SimulateStep({10, 20, 30}, {});

    auto pos = body.GetInterpolatedPosition(0.0f);
    EXPECT_NEAR(pos.x, 0.0f, 0.001f);
    EXPECT_NEAR(pos.y, 0.0f, 0.001f);
    EXPECT_NEAR(pos.z, 0.0f, 0.001f);
}

TEST(PhysicsInterp_AlphaOne_ReturnsCurrentPosition)
{
    TestPhysicsInterp::TestPhysicsBody body({0, 0, 0}, {});
    body.StoreCurrentState();
    body.SimulateStep({10, 20, 30}, {});

    auto pos = body.GetInterpolatedPosition(1.0f);
    EXPECT_NEAR(pos.x, 10.0f, 0.001f);
    EXPECT_NEAR(pos.y, 20.0f, 0.001f);
    EXPECT_NEAR(pos.z, 30.0f, 0.001f);
}

TEST(PhysicsInterp_AlphaHalf_ReturnsMidpoint)
{
    TestPhysicsInterp::TestPhysicsBody body({0, 0, 0}, {});
    body.StoreCurrentState();
    body.SimulateStep({10, 20, 30}, {});

    auto pos = body.GetInterpolatedPosition(0.5f);
    EXPECT_NEAR(pos.x, 5.0f, 0.001f);
    EXPECT_NEAR(pos.y, 10.0f, 0.001f);
    EXPECT_NEAR(pos.z, 15.0f, 0.001f);
}

TEST(PhysicsInterp_AlphaQuarter_ReturnsQuarterPoint)
{
    TestPhysicsInterp::TestPhysicsBody body({0, 0, 0}, {});
    body.StoreCurrentState();
    body.SimulateStep({8, 4, 12}, {});

    auto pos = body.GetInterpolatedPosition(0.25f);
    EXPECT_NEAR(pos.x, 2.0f, 0.001f);
    EXPECT_NEAR(pos.y, 1.0f, 0.001f);
    EXPECT_NEAR(pos.z, 3.0f, 0.001f);
}

TEST(PhysicsInterp_AccumulatorComputesAlpha)
{
    TestPhysicsInterp::TestPhysicsSystem system;
    system.m_timeStep = 1.0f / 60.0f; // ~0.01667s

    TestPhysicsInterp::TestPhysicsBody body({0, 0, 0}, {});
    TestPhysicsInterp::Vec3 velocity(60, 0, 0); // 60 units/sec = 1 unit per timestep

    // deltaTime exactly one timestep: accumulator should be 0, alpha should be 0
    system.Update(system.m_timeStep, body, velocity);
    EXPECT_NEAR(system.GetInterpolationAlpha(), 0.0f, 0.01f);

    // deltaTime half a timestep: accumulator should be half, alpha ~0.5
    system.m_accumulator = 0.0f;
    system.Update(system.m_timeStep * 0.5f, body, velocity);
    EXPECT_NEAR(system.GetInterpolationAlpha(), 0.5f, 0.01f);
}

TEST(PhysicsInterp_MultipleSteps_PositionAdvances)
{
    TestPhysicsInterp::TestPhysicsSystem system;
    system.m_timeStep = 1.0f / 60.0f;

    TestPhysicsInterp::TestPhysicsBody body({0, 0, 0}, {});
    TestPhysicsInterp::Vec3 velocity(60, 0, 0);

    // Run for 3 timesteps
    system.Update(system.m_timeStep * 3.0f, body, velocity);

    // Current position should be 3 units forward
    EXPECT_NEAR(body.m_currentPosition.x, 3.0f, 0.01f);
}

TEST(PhysicsInterp_StoreStateSavesPrevious)
{
    TestPhysicsInterp::TestPhysicsBody body({5, 10, 15}, {});
    body.SimulateStep({20, 30, 40}, {});
    body.StoreCurrentState();
    body.SimulateStep({50, 60, 70}, {});

    // Previous should now be {20, 30, 40}
    auto pos = body.GetInterpolatedPosition(0.0f);
    EXPECT_NEAR(pos.x, 20.0f, 0.001f);
    EXPECT_NEAR(pos.y, 30.0f, 0.001f);
    EXPECT_NEAR(pos.z, 40.0f, 0.001f);
}

TEST(PhysicsInterp_RotationSlerp_IdentityToIdentity)
{
    TestPhysicsInterp::Quat identity(0, 0, 0, 1);
    auto result = TestPhysicsInterp::Slerp(identity, identity, 0.5f);
    EXPECT_NEAR(result.w, 1.0f, 0.01f);
    EXPECT_NEAR(result.x, 0.0f, 0.01f);
    EXPECT_NEAR(result.y, 0.0f, 0.01f);
    EXPECT_NEAR(result.z, 0.0f, 0.01f);
}
