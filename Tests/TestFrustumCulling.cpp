/**
 * @file TestFrustumCulling.cpp
 * @brief Tests for frustum plane extraction and AABB/sphere visibility testing
 *
 * Uses standalone math to test the frustum culling algorithms without
 * requiring DirectX headers (for cross-platform CI builds).
 */

#include "TestFramework.h"
#include <cmath>
#include <array>

// ============================================================================
// Standalone reimplementation for testing (mirrors FrustumCulling.h logic)
// ============================================================================

namespace {

struct Vec3 { float x, y, z; };
struct AABB { Vec3 min, max; };
struct Sphere { Vec3 center; float radius; };

struct Plane {
    float a, b, c, d;

    void Normalize() {
        float len = std::sqrt(a*a + b*b + c*c);
        if (len > 0) { a /= len; b /= len; c /= len; d /= len; }
    }

    float Distance(const Vec3& p) const { return a*p.x + b*p.y + c*p.z + d; }
};

enum class CullResult { Outside, Inside, Intersecting };

// 4x4 row-major matrix (matching DirectX XMFLOAT4X4 layout)
struct Mat4 {
    float m[4][4]{};

    static Mat4 Perspective(float fovY, float aspect, float zNear, float zFar) {
        Mat4 r{};
        float h = 1.0f / std::tan(fovY * 0.5f);
        float w = h / aspect;
        r.m[0][0] = w;
        r.m[1][1] = h;
        r.m[2][2] = zFar / (zFar - zNear);
        r.m[2][3] = 1.0f;
        r.m[3][2] = -zNear * zFar / (zFar - zNear);
        r.m[3][3] = 0.0f;
        return r;
    }

    static Mat4 LookAt(Vec3 eye, Vec3 target, Vec3 up) {
        Vec3 f = { target.x - eye.x, target.y - eye.y, target.z - eye.z };
        float flen = std::sqrt(f.x*f.x + f.y*f.y + f.z*f.z);
        f = { f.x/flen, f.y/flen, f.z/flen };

        Vec3 s = { f.y*up.z - f.z*up.y, f.z*up.x - f.x*up.z, f.x*up.y - f.y*up.x };
        float slen = std::sqrt(s.x*s.x + s.y*s.y + s.z*s.z);
        s = { s.x/slen, s.y/slen, s.z/slen };

        Vec3 u = { s.y*f.z - s.z*f.y, s.z*f.x - s.x*f.z, s.x*f.y - s.y*f.x };

        Mat4 r{};
        r.m[0][0] = s.x;  r.m[0][1] = u.x;  r.m[0][2] = f.x;  r.m[0][3] = 0;
        r.m[1][0] = s.y;  r.m[1][1] = u.y;  r.m[1][2] = f.y;  r.m[1][3] = 0;
        r.m[2][0] = s.z;  r.m[2][1] = u.z;  r.m[2][2] = f.z;  r.m[2][3] = 0;
        r.m[3][0] = -(s.x*eye.x + s.y*eye.y + s.z*eye.z);
        r.m[3][1] = -(u.x*eye.x + u.y*eye.y + u.z*eye.z);
        r.m[3][2] = -(f.x*eye.x + f.y*eye.y + f.z*eye.z);
        r.m[3][3] = 1;
        return r;
    }

    static Mat4 Identity() {
        Mat4 r{};
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1;
        return r;
    }
};

Mat4 Multiply(const Mat4& a, const Mat4& b) {
    Mat4 r{};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++)
                r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

class Frustum {
public:
    void ExtractPlanes(const Mat4& m) {
        planes[0] = { m.m[0][3]+m.m[0][0], m.m[1][3]+m.m[1][0], m.m[2][3]+m.m[2][0], m.m[3][3]+m.m[3][0] }; // Left
        planes[1] = { m.m[0][3]-m.m[0][0], m.m[1][3]-m.m[1][0], m.m[2][3]-m.m[2][0], m.m[3][3]-m.m[3][0] }; // Right
        planes[2] = { m.m[0][3]+m.m[0][1], m.m[1][3]+m.m[1][1], m.m[2][3]+m.m[2][1], m.m[3][3]+m.m[3][1] }; // Bottom
        planes[3] = { m.m[0][3]-m.m[0][1], m.m[1][3]-m.m[1][1], m.m[2][3]-m.m[2][1], m.m[3][3]-m.m[3][1] }; // Top
        planes[4] = { m.m[0][2], m.m[1][2], m.m[2][2], m.m[3][2] }; // Near
        planes[5] = { m.m[0][3]-m.m[0][2], m.m[1][3]-m.m[1][2], m.m[2][3]-m.m[2][2], m.m[3][3]-m.m[3][2] }; // Far
        for (auto& p : planes) p.Normalize();
    }

    CullResult TestAABB(const AABB& box) const {
        bool allInside = true;
        for (int i = 0; i < 6; i++) {
            const auto& p = planes[i];
            Vec3 pv = { p.a >= 0 ? box.max.x : box.min.x,
                         p.b >= 0 ? box.max.y : box.min.y,
                         p.c >= 0 ? box.max.z : box.min.z };
            Vec3 nv = { p.a >= 0 ? box.min.x : box.max.x,
                         p.b >= 0 ? box.min.y : box.max.y,
                         p.c >= 0 ? box.min.z : box.max.z };
            if (p.Distance(pv) < 0) return CullResult::Outside;
            if (p.Distance(nv) < 0) allInside = false;
        }
        return allInside ? CullResult::Inside : CullResult::Intersecting;
    }

    CullResult TestSphere(const Sphere& s) const {
        bool allInside = true;
        for (int i = 0; i < 6; i++) {
            float d = planes[i].Distance(s.center);
            if (d < -s.radius) return CullResult::Outside;
            if (d < s.radius) allInside = false;
        }
        return allInside ? CullResult::Inside : CullResult::Intersecting;
    }

    bool IsPointInside(const Vec3& p) const {
        for (int i = 0; i < 6; i++)
            if (planes[i].Distance(p) < 0) return false;
        return true;
    }

    std::array<Plane, 6> planes;
};

} // anonymous namespace

// ============================================================================
// Tests
// ============================================================================

TEST(FrustumCulling_PointAtOriginVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-5}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f); // 60 deg FOV
    auto vp = Multiply(view, proj);
    f.ExtractPlanes(vp);

    EXPECT_TRUE(f.IsPointInside({0, 0, 0}));
}

TEST(FrustumCulling_PointBehindCameraNotVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-5}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    // Point far behind camera
    EXPECT_FALSE(f.IsPointInside({0, 0, -10}));
}

TEST(FrustumCulling_PointBeyondFarPlaneNotVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-5}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    // Point 200 units ahead — beyond far plane at 100
    EXPECT_FALSE(f.IsPointInside({0, 0, 200}));
}

TEST(FrustumCulling_AABBInFrontOfCameraVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-10}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    AABB box = {{-1,-1,-1}, {1,1,1}};
    auto result = f.TestAABB(box);
    EXPECT_TRUE(result != CullResult::Outside);
}

TEST(FrustumCulling_AABBBehindCameraNotVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-10}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    // Box entirely behind camera
    AABB box = {{-1,-1,-15}, {1,1,-12}};
    auto result = f.TestAABB(box);
    EXPECT_TRUE(result == CullResult::Outside);
}

TEST(FrustumCulling_AABBFarLeftNotVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-10}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    // Box way off to the left (x = -100 to -90)
    AABB box = {{-100,-1,0}, {-90,1,2}};
    auto result = f.TestAABB(box);
    EXPECT_TRUE(result == CullResult::Outside);
}

TEST(FrustumCulling_SphereAtOriginVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-10}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    Sphere s = {{0,0,0}, 1.0f};
    auto result = f.TestSphere(s);
    EXPECT_TRUE(result != CullResult::Outside);
}

TEST(FrustumCulling_SphereBehindCameraNotVisible) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-10}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    Sphere s = {{0,0,-20}, 1.0f};
    auto result = f.TestSphere(s);
    EXPECT_TRUE(result == CullResult::Outside);
}

TEST(FrustumCulling_LargeSphereIntersects) {
    Frustum f;
    auto view = Mat4::LookAt({0,0,-10}, {0,0,0}, {0,1,0});
    auto proj = Mat4::Perspective(1.0472f, 1.0f, 0.1f, 100.0f);
    f.ExtractPlanes(Multiply(view, proj));

    // Huge sphere partially inside
    Sphere s = {{50,0,0}, 100.0f};
    auto result = f.TestSphere(s);
    EXPECT_TRUE(result == CullResult::Inside || result == CullResult::Intersecting);
}

TEST(FrustumCulling_PlaneNormalization) {
    Plane p = {3, 0, 4, 10}; // length = 5
    p.Normalize();
    EXPECT_NEAR(p.a, 0.6f, 0.001f);
    EXPECT_NEAR(p.b, 0.0f, 0.001f);
    EXPECT_NEAR(p.c, 0.8f, 0.001f);
    EXPECT_NEAR(p.d, 2.0f, 0.001f);
}

TEST(FrustumCulling_PlaneDistance) {
    Plane p = {0, 1, 0, -5}; // y = 5 plane
    p.Normalize();
    float d = p.Distance({0, 10, 0});
    EXPECT_NEAR(d, 5.0f, 0.001f);
}
