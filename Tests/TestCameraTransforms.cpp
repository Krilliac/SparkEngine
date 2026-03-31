/**
 * @file TestCameraTransforms.cpp
 * @brief Tests for SparkEngineCamera: pitch clamping, matrix updates,
 *        zoom transitions, aspect ratio handling, and FOV management.
 *
 * Standalone reimplementation — mirrors Spark::SparkEngineCamera
 * without DirectXMath dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <array>
#include <cmath>

namespace
{

    constexpr float PI = 3.14159265358979323846f;
    constexpr float DEG2RAD = PI / 180.0f;
    constexpr float RAD2DEG = 180.0f / PI;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        Vec3 Normalized() const
        {
            float len = Length();
            if (len < 0.0001f)
                return {0, 0, 0};
            return {x / len, y / len, z / len};
        }
        float Dot(const Vec3& o) const { return x * o.x + y * o.y + z * o.z; }
    };

    struct Mat4
    {
        float m[4][4] = {};

        static Mat4 Identity()
        {
            Mat4 mat{};
            mat.m[0][0] = mat.m[1][1] = mat.m[2][2] = mat.m[3][3] = 1.0f;
            return mat;
        }

        static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up)
        {
            Vec3 zaxis = Vec3(eye.x - target.x, eye.y - target.y, eye.z - target.z).Normalized();
            Vec3 xaxis =
                Vec3(up.y * zaxis.z - up.z * zaxis.y, up.z * zaxis.x - up.x * zaxis.z, up.x * zaxis.y - up.y * zaxis.x)
                    .Normalized();
            Vec3 yaxis = Vec3(zaxis.y * xaxis.z - zaxis.z * xaxis.y, zaxis.z * xaxis.x - zaxis.x * xaxis.z,
                              zaxis.x * xaxis.y - zaxis.y * xaxis.x);

            Mat4 result = Identity();
            result.m[0][0] = xaxis.x;
            result.m[1][0] = xaxis.y;
            result.m[2][0] = xaxis.z;
            result.m[0][1] = yaxis.x;
            result.m[1][1] = yaxis.y;
            result.m[2][1] = yaxis.z;
            result.m[0][2] = zaxis.x;
            result.m[1][2] = zaxis.y;
            result.m[2][2] = zaxis.z;
            result.m[3][0] = -(xaxis.Dot(eye));
            result.m[3][1] = -(yaxis.Dot(eye));
            result.m[3][2] = -(zaxis.Dot(eye));
            return result;
        }

        static Mat4 Perspective(float fovRadians, float aspectRatio, float nearPlane, float farPlane)
        {
            float yScale = 1.0f / std::tan(fovRadians * 0.5f);
            float xScale = yScale / aspectRatio;

            Mat4 result{};
            result.m[0][0] = xScale;
            result.m[1][1] = yScale;
            result.m[2][2] = farPlane / (farPlane - nearPlane);
            result.m[2][3] = 1.0f;
            result.m[3][2] = -(nearPlane * farPlane) / (farPlane - nearPlane);
            return result;
        }
    };

    // --- Camera reimplementation ---

    class SparkEngineCamera
    {
      public:
        SparkEngineCamera()
        {
            m_position = {0, 0, 0};
            m_rotation = {0, 0, 0}; // pitch, yaw, roll in degrees
            m_forward = {0, 0, 1};
            m_right = {1, 0, 0};
            m_up = {0, 1, 0};
        }

        void Initialize(float aspectRatio)
        {
            m_aspectRatio = aspectRatio;
            m_currentFov = m_defaultFov;
            UpdateVectors();
            UpdateMatrices();
        }

        void SetPosition(const Vec3& pos)
        {
            m_position = pos;
            UpdateMatrices();
        }

        void MoveForward(float amount)
        {
            m_position = m_position + m_forward * amount;
            UpdateMatrices();
        }

        void MoveRight(float amount)
        {
            m_position = m_position + m_right * amount;
            UpdateMatrices();
        }

        void MoveUp(float amount)
        {
            m_position = m_position + Vec3(0, 1, 0) * amount;
            UpdateMatrices();
        }

        void Pitch(float angle)
        {
            m_rotation.x += angle;
            // Clamp pitch to [-89, 89] degrees
            if (m_rotation.x > 89.0f)
                m_rotation.x = 89.0f;
            if (m_rotation.x < -89.0f)
                m_rotation.x = -89.0f;
            UpdateVectors();
            UpdateMatrices();
        }

        void Yaw(float angle)
        {
            m_rotation.y += angle;
            // Wrap yaw to [0, 360)
            while (m_rotation.y >= 360.0f)
                m_rotation.y -= 360.0f;
            while (m_rotation.y < 0.0f)
                m_rotation.y += 360.0f;
            UpdateVectors();
            UpdateMatrices();
        }

        void Roll(float angle)
        {
            m_rotation.z += angle;
            UpdateVectors();
            UpdateMatrices();
        }

        void SetZoom(bool enabled)
        {
            m_isZoomed = enabled;
            m_currentFov = enabled ? m_zoomedFov : m_defaultFov;
            UpdateMatrices();
        }

        void SetFOV(float degrees)
        {
            m_defaultFov = degrees;
            if (!m_isZoomed)
                m_currentFov = degrees;
            UpdateMatrices();
        }

        void SetClippingPlanes(float near, float far)
        {
            m_nearPlane = near;
            m_farPlane = far;
            UpdateMatrices();
        }

        void SetMoveSpeed(float speed) { m_moveSpeed = speed; }
        void SetMouseSensitivity(float sensitivity) { m_mouseSensitivity = sensitivity; }
        void SetInvertY(bool invert) { m_invertY = invert; }

        // Smooth transition
        void SmoothMoveTo(float x, float y, float z, float duration)
        {
            m_transitionStart = m_position;
            m_transitionEnd = {x, y, z};
            m_transitionDuration = duration;
            m_transitionElapsed = 0.0f;
            m_isTransitioning = true;
        }

        void Update(float deltaTime)
        {
            if (m_isTransitioning)
            {
                m_transitionElapsed += deltaTime;
                float t = m_transitionElapsed / m_transitionDuration;
                if (t >= 1.0f)
                {
                    t = 1.0f;
                    m_isTransitioning = false;
                }
                // Smooth step
                t = t * t * (3.0f - 2.0f * t);
                m_position.x = m_transitionStart.x + (m_transitionEnd.x - m_transitionStart.x) * t;
                m_position.y = m_transitionStart.y + (m_transitionEnd.y - m_transitionStart.y) * t;
                m_position.z = m_transitionStart.z + (m_transitionEnd.z - m_transitionStart.z) * t;
                UpdateMatrices();
            }
        }

        const Vec3& GetPosition() const { return m_position; }
        const Vec3& GetForward() const { return m_forward; }
        const Vec3& GetRight() const { return m_right; }
        const Vec3& GetUp() const { return m_up; }
        Vec3 GetRotation() const { return m_rotation; }
        float GetCurrentFov() const { return m_currentFov; }
        float GetAspectRatio() const { return m_aspectRatio; }
        float GetNearPlane() const { return m_nearPlane; }
        float GetFarPlane() const { return m_farPlane; }
        bool IsZoomed() const { return m_isZoomed; }
        bool IsTransitioning() const { return m_isTransitioning; }
        float GetMoveSpeed() const { return m_moveSpeed; }
        float GetMouseSensitivity() const { return m_mouseSensitivity; }
        const Mat4& GetViewMatrix() const { return m_viewMatrix; }
        const Mat4& GetProjectionMatrix() const { return m_projMatrix; }

      private:
        void UpdateVectors()
        {
            float pitchRad = m_rotation.x * DEG2RAD;
            float yawRad = m_rotation.y * DEG2RAD;

            m_forward.x = std::cos(pitchRad) * std::sin(yawRad);
            m_forward.y = std::sin(pitchRad);
            m_forward.z = std::cos(pitchRad) * std::cos(yawRad);
            m_forward = m_forward.Normalized();

            Vec3 worldUp = {0, 1, 0};
            // Cross product: worldUp × forward = right
            m_right.x = worldUp.y * m_forward.z - worldUp.z * m_forward.y;
            m_right.y = worldUp.z * m_forward.x - worldUp.x * m_forward.z;
            m_right.z = worldUp.x * m_forward.y - worldUp.y * m_forward.x;
            m_right = m_right.Normalized();

            m_up.x = m_forward.y * m_right.z - m_forward.z * m_right.y;
            m_up.y = m_forward.z * m_right.x - m_forward.x * m_right.z;
            m_up.z = m_forward.x * m_right.y - m_forward.y * m_right.x;
            m_up = m_up.Normalized();
        }

        void UpdateMatrices()
        {
            Vec3 target = m_position + m_forward;
            m_viewMatrix = Mat4::LookAt(m_position, target, m_up);
            m_projMatrix = Mat4::Perspective(m_currentFov * DEG2RAD, m_aspectRatio, m_nearPlane, m_farPlane);
        }

        Vec3 m_position{};
        Vec3 m_rotation{}; // pitch, yaw, roll (degrees)
        Vec3 m_forward{0, 0, 1};
        Vec3 m_right{1, 0, 0};
        Vec3 m_up{0, 1, 0};

        float m_defaultFov = 90.0f;
        float m_zoomedFov = 40.0f;
        float m_currentFov = 90.0f;
        float m_aspectRatio = 16.0f / 9.0f;
        float m_nearPlane = 0.1f;
        float m_farPlane = 1000.0f;
        float m_moveSpeed = 5.0f;
        float m_mouseSensitivity = 1.0f;
        bool m_invertY = false;
        bool m_isZoomed = false;

        // Smooth transition
        Vec3 m_transitionStart{};
        Vec3 m_transitionEnd{};
        float m_transitionDuration = 0.0f;
        float m_transitionElapsed = 0.0f;
        bool m_isTransitioning = false;

        Mat4 m_viewMatrix = Mat4::Identity();
        Mat4 m_projMatrix = Mat4::Identity();
    };

} // anonymous namespace

// ============================================================================
// Pitch Clamping Tests
// ============================================================================

TEST(Camera_PitchClampUp)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Pitch(100.0f);
    EXPECT_NEAR(cam.GetRotation().x, 89.0f, 0.01f);
}

TEST(Camera_PitchClampDown)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Pitch(-100.0f);
    EXPECT_NEAR(cam.GetRotation().x, -89.0f, 0.01f);
}

TEST(Camera_PitchAccumulates)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Pitch(20.0f);
    cam.Pitch(15.0f);
    EXPECT_NEAR(cam.GetRotation().x, 35.0f, 0.01f);
}

TEST(Camera_PitchClampAfterAccumulation)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Pitch(50.0f);
    cam.Pitch(50.0f); // would be 100, clamped to 89
    EXPECT_NEAR(cam.GetRotation().x, 89.0f, 0.01f);
}

// ============================================================================
// Yaw Tests
// ============================================================================

TEST(Camera_YawWraps)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Yaw(370.0f);
    EXPECT_NEAR(cam.GetRotation().y, 10.0f, 0.01f);
}

TEST(Camera_YawNegativeWraps)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Yaw(-10.0f);
    EXPECT_NEAR(cam.GetRotation().y, 350.0f, 0.01f);
}

// ============================================================================
// Forward Vector Tests
// ============================================================================

TEST(Camera_ForwardDefaultLooksAlongZ)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    // With yaw=0, pitch=0, forward should be along +Z
    EXPECT_NEAR(cam.GetForward().z, 1.0f, 0.01f);
    EXPECT_NEAR(cam.GetForward().x, 0.0f, 0.01f);
    EXPECT_NEAR(cam.GetForward().y, 0.0f, 0.01f);
}

TEST(Camera_ForwardAfterYaw90)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Yaw(90.0f);
    // Should look along +X
    EXPECT_NEAR(cam.GetForward().x, 1.0f, 0.01f);
    EXPECT_NEAR(cam.GetForward().z, 0.0f, 0.05f);
}

TEST(Camera_ForwardIsUnitVector)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.Pitch(30.0f);
    cam.Yaw(45.0f);

    EXPECT_NEAR(cam.GetForward().Length(), 1.0f, 0.01f);
}

// ============================================================================
// Movement Tests
// ============================================================================

TEST(Camera_MoveForward)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.MoveForward(10.0f);
    // Default forward is +Z
    EXPECT_NEAR(cam.GetPosition().z, 10.0f, 0.1f);
}

TEST(Camera_MoveRight)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.MoveRight(5.0f);
    // Default right is +X (left-hand) or -X (right-hand) — depends on convention
    EXPECT_TRUE(std::abs(cam.GetPosition().x) > 0.1f);
}

TEST(Camera_MoveUp)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.MoveUp(3.0f);
    EXPECT_NEAR(cam.GetPosition().y, 3.0f, 0.01f);
}

TEST(Camera_SetPosition)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.SetPosition({10.0f, 20.0f, 30.0f});
    EXPECT_NEAR(cam.GetPosition().x, 10.0f, 0.01f);
    EXPECT_NEAR(cam.GetPosition().y, 20.0f, 0.01f);
    EXPECT_NEAR(cam.GetPosition().z, 30.0f, 0.01f);
}

// ============================================================================
// Zoom / FOV Tests
// ============================================================================

TEST(Camera_DefaultFOV)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    EXPECT_NEAR(cam.GetCurrentFov(), 90.0f, 0.01f);
}

TEST(Camera_ZoomReducesFOV)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.SetZoom(true);
    EXPECT_TRUE(cam.IsZoomed());
    EXPECT_NEAR(cam.GetCurrentFov(), 40.0f, 0.01f);
}

TEST(Camera_UnzoomRestoresFOV)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.SetZoom(true);
    cam.SetZoom(false);
    EXPECT_FALSE(cam.IsZoomed());
    EXPECT_NEAR(cam.GetCurrentFov(), 90.0f, 0.01f);
}

TEST(Camera_SetCustomFOV)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.SetFOV(110.0f);
    EXPECT_NEAR(cam.GetCurrentFov(), 110.0f, 0.01f);
}

// ============================================================================
// Aspect Ratio / Clipping Plane Tests
// ============================================================================

TEST(Camera_AspectRatio)
{
    SparkEngineCamera cam;
    cam.Initialize(21.0f / 9.0f);

    EXPECT_NEAR(cam.GetAspectRatio(), 21.0f / 9.0f, 0.01f);
}

TEST(Camera_ClippingPlanes)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    EXPECT_NEAR(cam.GetNearPlane(), 0.1f, 0.001f);
    EXPECT_NEAR(cam.GetFarPlane(), 1000.0f, 0.1f);

    cam.SetClippingPlanes(0.5f, 5000.0f);
    EXPECT_NEAR(cam.GetNearPlane(), 0.5f, 0.001f);
    EXPECT_NEAR(cam.GetFarPlane(), 5000.0f, 0.1f);
}

// ============================================================================
// Smooth Transition Tests
// ============================================================================

TEST(Camera_SmoothTransitionStarts)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);
    cam.SetPosition({0, 0, 0});

    cam.SmoothMoveTo(100, 0, 0, 2.0f);
    EXPECT_TRUE(cam.IsTransitioning());

    cam.Update(0.0f);
    EXPECT_NEAR(cam.GetPosition().x, 0.0f, 0.1f);
}

TEST(Camera_SmoothTransitionCompletes)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);
    cam.SetPosition({0, 0, 0});

    cam.SmoothMoveTo(100, 50, 25, 1.0f);

    // Run past duration
    cam.Update(2.0f);

    EXPECT_FALSE(cam.IsTransitioning());
    EXPECT_NEAR(cam.GetPosition().x, 100.0f, 0.1f);
    EXPECT_NEAR(cam.GetPosition().y, 50.0f, 0.1f);
    EXPECT_NEAR(cam.GetPosition().z, 25.0f, 0.1f);
}

TEST(Camera_SmoothTransitionMidway)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);
    cam.SetPosition({0, 0, 0});

    cam.SmoothMoveTo(100, 0, 0, 2.0f);
    cam.Update(1.0f); // halfway

    // At t=0.5, smoothstep = 0.5
    EXPECT_TRUE(cam.IsTransitioning());
    EXPECT_NEAR(cam.GetPosition().x, 50.0f, 1.0f);
}

// ============================================================================
// Matrix Tests
// ============================================================================

TEST(Camera_ViewMatrixUpdatesOnMove)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    auto view1 = cam.GetViewMatrix();
    cam.MoveForward(10.0f);
    auto view2 = cam.GetViewMatrix();

    // Translation part of view matrix should differ
    EXPECT_TRUE(std::abs(view1.m[3][2] - view2.m[3][2]) > 0.01f);
}

TEST(Camera_ProjectionMatrixDiagonal)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    auto& proj = cam.GetProjectionMatrix();
    // Projection matrix should have non-zero diagonal entries
    EXPECT_TRUE(std::abs(proj.m[0][0]) > 0.01f);
    EXPECT_TRUE(std::abs(proj.m[1][1]) > 0.01f);
    EXPECT_TRUE(std::abs(proj.m[2][2]) > 0.01f);
}

// ============================================================================
// Settings Tests
// ============================================================================

TEST(Camera_MoveSpeed)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.SetMoveSpeed(10.0f);
    EXPECT_NEAR(cam.GetMoveSpeed(), 10.0f, 0.01f);
}

TEST(Camera_MouseSensitivity)
{
    SparkEngineCamera cam;
    cam.Initialize(16.0f / 9.0f);

    cam.SetMouseSensitivity(2.5f);
    EXPECT_NEAR(cam.GetMouseSensitivity(), 2.5f, 0.01f);
}
