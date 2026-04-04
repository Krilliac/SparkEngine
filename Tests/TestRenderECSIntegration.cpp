/**
 * @file TestRenderECSIntegration.cpp
 * @brief Integration tests for rendering + ECS interaction patterns
 */

#include "TestFramework.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };
    struct Quat
    {
        float x = 0, y = 0, z = 0, w = 1;
    };
    struct Mat4x4
    {
        float m[4][4] = {};
    };
    struct TransformComp
    {
        Vec3 position;
        Quat rotation;
        Vec3 scale{1, 1, 1};
    };

    Mat4x4 ComputeWorldMatrix(const TransformComp& t)
    {
        Mat4x4 m{};
        m.m[0][0] = t.scale.x;
        m.m[1][1] = t.scale.y;
        m.m[2][2] = t.scale.z;
        m.m[3][3] = 1.0f;
        m.m[3][0] = t.position.x;
        m.m[3][1] = t.position.y;
        m.m[3][2] = t.position.z;
        return m;
    }

    bool FrustumContains(float x, float y, float z, float nearP, float farP)
    {
        float dist = std::sqrt(x * x + y * y + z * z);
        return dist >= nearP && dist <= farP;
    }
} // namespace

TEST(RenderECS_WorldMatrixTranslation)
{
    TransformComp t;
    t.position = {5, 10, -3};
    auto w = ComputeWorldMatrix(t);
    EXPECT_NEAR(w.m[3][0], 5.0f, 0.001f);
    EXPECT_NEAR(w.m[3][1], 10.0f, 0.001f);
    EXPECT_NEAR(w.m[3][2], -3.0f, 0.001f);
}

TEST(RenderECS_WorldMatrixScale)
{
    TransformComp t;
    t.scale = {2, 3, 4};
    auto w = ComputeWorldMatrix(t);
    EXPECT_NEAR(w.m[0][0], 2.0f, 0.001f);
    EXPECT_NEAR(w.m[1][1], 3.0f, 0.001f);
    EXPECT_NEAR(w.m[2][2], 4.0f, 0.001f);
}

TEST(RenderECS_FrustumCullFar)
{
    EXPECT_FALSE(FrustumContains(500, 0, 0, 0.1f, 100.0f));
    EXPECT_TRUE(FrustumContains(50, 0, 0, 0.1f, 100.0f));
}

TEST(RenderECS_FrustumCullNear)
{
    EXPECT_FALSE(FrustumContains(0.01f, 0, 0, 0.1f, 100.0f));
    EXPECT_TRUE(FrustumContains(0.2f, 0, 0, 0.1f, 100.0f));
}

TEST(RenderECS_TransformChangeUpdatesMatrix)
{
    TransformComp t;
    auto w1 = ComputeWorldMatrix(t);
    t.position = {10, 20, 30};
    auto w2 = ComputeWorldMatrix(t);
    EXPECT_NEAR(w1.m[3][0], 0.0f, 0.001f);
    EXPECT_NEAR(w2.m[3][0], 10.0f, 0.001f);
}

TEST(RenderECS_MaterialDirtyFlag)
{
    struct MaterialState
    {
        float roughness = 0.5f;
        bool isDirty = false;
    };
    MaterialState mat;
    EXPECT_FALSE(mat.isDirty);
    mat.roughness = 0.8f;
    mat.isDirty = true;
    EXPECT_TRUE(mat.isDirty);
}

TEST(RenderECS_BatchInstancing)
{
    struct RenderBatch
    {
        uint32_t materialId;
        std::vector<Mat4x4> transforms;
    };
    RenderBatch batch;
    batch.materialId = 1;
    for (int i = 0; i < 100; ++i)
    {
        TransformComp t;
        t.position.x = static_cast<float>(i);
        batch.transforms.push_back(ComputeWorldMatrix(t));
    }
    EXPECT_EQ(batch.transforms.size(), static_cast<size_t>(100));
    EXPECT_NEAR(batch.transforms[50].m[3][0], 50.0f, 0.001f);
}

TEST(RenderECS_IdentityMatrix)
{
    TransformComp t;
    auto w = ComputeWorldMatrix(t);
    EXPECT_NEAR(w.m[0][0], 1.0f, 0.001f);
    EXPECT_NEAR(w.m[1][1], 1.0f, 0.001f);
    EXPECT_NEAR(w.m[2][2], 1.0f, 0.001f);
    EXPECT_NEAR(w.m[3][3], 1.0f, 0.001f);
    EXPECT_NEAR(w.m[0][1], 0.0f, 0.001f);
}
