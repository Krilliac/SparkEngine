/**
 * @file TestGameObjectTransforms.cpp
 * @brief Tests for GameObject: world matrix computation, direction vectors,
 *        transform chain operations, distance calculation, and active/visible state.
 *
 * Standalone reimplementation — mirrors Spark::GameObject
 * without D3D11/DirectXMath dependencies for CI/Linux compatibility.
 */

#include "TestFramework.h"

#include <cmath>
#include <string>

namespace
{

    constexpr float PI = 3.14159265358979323846f;
    constexpr float DEG2RAD = PI / 180.0f;

    struct Vec3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;

        Vec3() = default;
        Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

        Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
        Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
        Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }

        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        Vec3 Normalized() const
        {
            float len = Length();
            if (len < 0.0001f)
                return {0, 0, 0};
            return {x / len, y / len, z / len};
        }
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
    };

    // --- GameObject reimplementation ---

    class GameObject
    {
      public:
        GameObject() : m_id(s_nextID++) {}
        virtual ~GameObject() = default;

        void SetPosition(const Vec3& pos)
        {
            m_position = pos;
            m_worldDirty = true;
        }

        void SetRotation(const Vec3& rot)
        {
            m_rotation = rot;
            m_worldDirty = true;
        }

        void SetScale(const Vec3& scale)
        {
            m_scale = scale;
            m_worldDirty = true;
        }

        void Translate(const Vec3& delta)
        {
            m_position = m_position + delta;
            m_worldDirty = true;
        }

        void Rotate(const Vec3& delta)
        {
            m_rotation = m_rotation + delta;
            m_worldDirty = true;
        }

        void Scale(const Vec3& delta)
        {
            m_scale.x *= delta.x;
            m_scale.y *= delta.y;
            m_scale.z *= delta.z;
            m_worldDirty = true;
        }

        const Vec3& GetPosition() const { return m_position; }
        const Vec3& GetRotation() const { return m_rotation; }
        const Vec3& GetScale() const { return m_scale; }

        Mat4 GetWorldMatrix() const
        {
            if (m_worldDirty)
                UpdateWorldMatrix();
            return m_worldMatrix;
        }

        Vec3 GetForward() const
        {
            float pitchRad = m_rotation.x * DEG2RAD;
            float yawRad = m_rotation.y * DEG2RAD;
            return Vec3(std::cos(pitchRad) * std::sin(yawRad), std::sin(pitchRad),
                        std::cos(pitchRad) * std::cos(yawRad))
                .Normalized();
        }

        Vec3 GetRight() const
        {
            float yawRad = m_rotation.y * DEG2RAD;
            return Vec3(std::cos(yawRad), 0.0f, -std::sin(yawRad)).Normalized();
        }

        Vec3 GetUp() const
        {
            // Simplified — true up is cross(forward, right)
            return Vec3(0, 1, 0);
        }

        bool IsActive() const { return m_active; }
        bool IsVisible() const { return m_visible; }
        void SetActive(bool v) { m_active = v; }
        void SetVisible(bool v) { m_visible = v; }

        unsigned int GetID() const { return m_id; }
        const std::string& GetName() const { return m_name; }
        void SetName(const std::string& n) { m_name = n; }

        float GetDistanceFrom(const GameObject& other) const { return (m_position - other.m_position).Length(); }

        float GetDistanceFrom(const Vec3& point) const { return (m_position - point).Length(); }

      protected:
        void UpdateWorldMatrix() const
        {
            m_worldMatrix = Mat4::Identity();

            // Scale
            m_worldMatrix.m[0][0] = m_scale.x;
            m_worldMatrix.m[1][1] = m_scale.y;
            m_worldMatrix.m[2][2] = m_scale.z;

            // Rotation (simplified — Y rotation only for testing)
            float yawRad = m_rotation.y * DEG2RAD;
            float cosY = std::cos(yawRad);
            float sinY = std::sin(yawRad);
            float sx = m_worldMatrix.m[0][0];
            float sz = m_worldMatrix.m[2][2];
            m_worldMatrix.m[0][0] = cosY * sx;
            m_worldMatrix.m[0][2] = sinY * sz;
            m_worldMatrix.m[2][0] = -sinY * sx;
            m_worldMatrix.m[2][2] = cosY * sz;

            // Translation
            m_worldMatrix.m[3][0] = m_position.x;
            m_worldMatrix.m[3][1] = m_position.y;
            m_worldMatrix.m[3][2] = m_position.z;

            m_worldDirty = false;
        }

        Vec3 m_position{0, 0, 0};
        Vec3 m_rotation{0, 0, 0};
        Vec3 m_scale{1, 1, 1};
        bool m_active = true;
        bool m_visible = true;
        unsigned int m_id = 0;
        std::string m_name;

        mutable Mat4 m_worldMatrix = Mat4::Identity();
        mutable bool m_worldDirty = true;

        static inline unsigned int s_nextID = 1;
    };

} // anonymous namespace

// ============================================================================
// Position Tests
// ============================================================================

TEST(GameObject_DefaultPosition)
{
    GameObject obj;
    EXPECT_NEAR(obj.GetPosition().x, 0.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().y, 0.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().z, 0.0f, 0.01f);
}

TEST(GameObject_SetPosition)
{
    GameObject obj;
    obj.SetPosition({10, 20, 30});
    EXPECT_NEAR(obj.GetPosition().x, 10.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().y, 20.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().z, 30.0f, 0.01f);
}

TEST(GameObject_Translate)
{
    GameObject obj;
    obj.SetPosition({5, 0, 0});
    obj.Translate({3, 2, 1});
    EXPECT_NEAR(obj.GetPosition().x, 8.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().y, 2.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().z, 1.0f, 0.01f);
}

TEST(GameObject_MultipleTranslates)
{
    GameObject obj;
    obj.Translate({1, 0, 0});
    obj.Translate({0, 1, 0});
    obj.Translate({0, 0, 1});
    EXPECT_NEAR(obj.GetPosition().x, 1.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().y, 1.0f, 0.01f);
    EXPECT_NEAR(obj.GetPosition().z, 1.0f, 0.01f);
}

// ============================================================================
// Rotation Tests
// ============================================================================

TEST(GameObject_SetRotation)
{
    GameObject obj;
    obj.SetRotation({45, 90, 0});
    EXPECT_NEAR(obj.GetRotation().x, 45.0f, 0.01f);
    EXPECT_NEAR(obj.GetRotation().y, 90.0f, 0.01f);
}

TEST(GameObject_Rotate)
{
    GameObject obj;
    obj.SetRotation({10, 20, 0});
    obj.Rotate({5, 10, 0});
    EXPECT_NEAR(obj.GetRotation().x, 15.0f, 0.01f);
    EXPECT_NEAR(obj.GetRotation().y, 30.0f, 0.01f);
}

// ============================================================================
// Scale Tests
// ============================================================================

TEST(GameObject_DefaultScale)
{
    GameObject obj;
    EXPECT_NEAR(obj.GetScale().x, 1.0f, 0.01f);
    EXPECT_NEAR(obj.GetScale().y, 1.0f, 0.01f);
    EXPECT_NEAR(obj.GetScale().z, 1.0f, 0.01f);
}

TEST(GameObject_SetScale)
{
    GameObject obj;
    obj.SetScale({2, 3, 4});
    EXPECT_NEAR(obj.GetScale().x, 2.0f, 0.01f);
    EXPECT_NEAR(obj.GetScale().y, 3.0f, 0.01f);
    EXPECT_NEAR(obj.GetScale().z, 4.0f, 0.01f);
}

TEST(GameObject_MultiplicativeScale)
{
    GameObject obj;
    obj.SetScale({2, 2, 2});
    obj.Scale({3, 3, 3});
    EXPECT_NEAR(obj.GetScale().x, 6.0f, 0.01f);
    EXPECT_NEAR(obj.GetScale().y, 6.0f, 0.01f);
    EXPECT_NEAR(obj.GetScale().z, 6.0f, 0.01f);
}

// ============================================================================
// Direction Vector Tests
// ============================================================================

TEST(GameObject_ForwardDefault)
{
    GameObject obj;
    auto fwd = obj.GetForward();
    EXPECT_NEAR(fwd.z, 1.0f, 0.01f);
    EXPECT_NEAR(fwd.x, 0.0f, 0.01f);
}

TEST(GameObject_ForwardAfterYaw90)
{
    GameObject obj;
    obj.SetRotation({0, 90, 0});
    auto fwd = obj.GetForward();
    EXPECT_NEAR(fwd.x, 1.0f, 0.01f);
    EXPECT_NEAR(fwd.z, 0.0f, 0.05f);
}

TEST(GameObject_ForwardIsUnitVector)
{
    GameObject obj;
    obj.SetRotation({30, 45, 0});
    auto fwd = obj.GetForward();
    EXPECT_NEAR(fwd.Length(), 1.0f, 0.01f);
}

TEST(GameObject_RightPerpendicularToForward)
{
    GameObject obj;
    obj.SetRotation({0, 45, 0});
    auto fwd = obj.GetForward();
    auto right = obj.GetRight();

    // Dot product should be ~0 (perpendicular)
    float dot = fwd.x * right.x + fwd.y * right.y + fwd.z * right.z;
    EXPECT_NEAR(dot, 0.0f, 0.05f);
}

// ============================================================================
// World Matrix Tests
// ============================================================================

TEST(GameObject_WorldMatrixTranslation)
{
    GameObject obj;
    obj.SetPosition({10, 20, 30});

    auto mat = obj.GetWorldMatrix();
    EXPECT_NEAR(mat.m[3][0], 10.0f, 0.01f);
    EXPECT_NEAR(mat.m[3][1], 20.0f, 0.01f);
    EXPECT_NEAR(mat.m[3][2], 30.0f, 0.01f);
}

TEST(GameObject_WorldMatrixScale)
{
    GameObject obj;
    obj.SetScale({2, 3, 4});

    auto mat = obj.GetWorldMatrix();
    // Diagonal should reflect scale (before rotation)
    EXPECT_NEAR(mat.m[0][0], 2.0f, 0.01f);
    EXPECT_NEAR(mat.m[1][1], 3.0f, 0.01f);
    EXPECT_NEAR(mat.m[2][2], 4.0f, 0.01f);
}

TEST(GameObject_WorldMatrixIdentityAtDefault)
{
    GameObject obj;
    auto mat = obj.GetWorldMatrix();

    EXPECT_NEAR(mat.m[0][0], 1.0f, 0.01f);
    EXPECT_NEAR(mat.m[1][1], 1.0f, 0.01f);
    EXPECT_NEAR(mat.m[2][2], 1.0f, 0.01f);
    EXPECT_NEAR(mat.m[3][3], 1.0f, 0.01f);
    EXPECT_NEAR(mat.m[3][0], 0.0f, 0.01f);
}

// ============================================================================
// Distance Tests
// ============================================================================

TEST(GameObject_DistanceBetween)
{
    GameObject a, b;
    a.SetPosition({0, 0, 0});
    b.SetPosition({3, 4, 0});

    EXPECT_NEAR(a.GetDistanceFrom(b), 5.0f, 0.01f);
}

TEST(GameObject_DistanceFromPoint)
{
    GameObject obj;
    obj.SetPosition({1, 1, 1});

    float dist = obj.GetDistanceFrom(Vec3(4, 5, 1));
    EXPECT_NEAR(dist, 5.0f, 0.01f);
}

TEST(GameObject_DistanceZero)
{
    GameObject a, b;
    a.SetPosition({5, 5, 5});
    b.SetPosition({5, 5, 5});

    EXPECT_NEAR(a.GetDistanceFrom(b), 0.0f, 0.01f);
}

// ============================================================================
// Active/Visible State Tests
// ============================================================================

TEST(GameObject_ActiveByDefault)
{
    GameObject obj;
    EXPECT_TRUE(obj.IsActive());
    EXPECT_TRUE(obj.IsVisible());
}

TEST(GameObject_SetActive)
{
    GameObject obj;
    obj.SetActive(false);
    EXPECT_FALSE(obj.IsActive());

    obj.SetActive(true);
    EXPECT_TRUE(obj.IsActive());
}

TEST(GameObject_SetVisible)
{
    GameObject obj;
    obj.SetVisible(false);
    EXPECT_FALSE(obj.IsVisible());
}

// ============================================================================
// Identity / Name Tests
// ============================================================================

TEST(GameObject_UniqueIDs)
{
    GameObject a, b, c;
    EXPECT_NE(a.GetID(), b.GetID());
    EXPECT_NE(b.GetID(), c.GetID());
}

TEST(GameObject_SetName)
{
    GameObject obj;
    obj.SetName("Player");
    EXPECT_EQ(obj.GetName(), std::string("Player"));
}
