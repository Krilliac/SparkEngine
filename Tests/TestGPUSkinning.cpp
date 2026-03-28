// TestGPUSkinning.cpp - Tests for GPU compute skinning logic
// Validates bone matrix application and vertex transformation without D3D11

#include "TestFramework.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

    struct SourceVertex
    {
        float position[3];
        float normal[3];
        float texCoord[2];
        float tangent[3];
        float pad0;
        uint32_t boneIndices[4];
        float boneWeights[4];
    };

    struct SkinnedVertex
    {
        float position[3];
        float normal[3];
        float texCoord[2];
        float tangent[3];
        float pad0;
    };

    // 4x4 identity matrix stored as 3 float4 rows (compact bone format)
    struct BoneMatrix
    {
        float rows[3][4];
    };

    BoneMatrix IdentityBone()
    {
        BoneMatrix m = {};
        m.rows[0][0] = 1.0f;
        m.rows[1][1] = 1.0f;
        m.rows[2][2] = 1.0f;
        return m;
    }

    BoneMatrix TranslationBone(float x, float y, float z)
    {
        BoneMatrix m = IdentityBone();
        m.rows[0][3] = x;
        m.rows[1][3] = y;
        m.rows[2][3] = z;
        return m;
    }

    BoneMatrix ScaleBone(float s)
    {
        BoneMatrix m = {};
        m.rows[0][0] = s;
        m.rows[1][1] = s;
        m.rows[2][2] = s;
        return m;
    }

    // CPU reference skinning (mirrors SkinningCS.hlsl)
    SkinnedVertex SkinVertex(const SourceVertex& src, const std::vector<BoneMatrix>& bones)
    {
        float skinnedPos[3] = {0, 0, 0};
        float skinnedNormal[3] = {0, 0, 0};

        for (int i = 0; i < 4; i++)
        {
            float weight = src.boneWeights[i];
            if (weight <= 0.0f)
                continue;

            uint32_t boneIdx = src.boneIndices[i];
            if (boneIdx >= bones.size())
                continue;

            const auto& b = bones[boneIdx];

            // Transform position (with translation)
            float px = b.rows[0][0] * src.position[0] + b.rows[0][1] * src.position[1] +
                       b.rows[0][2] * src.position[2] + b.rows[0][3];
            float py = b.rows[1][0] * src.position[0] + b.rows[1][1] * src.position[1] +
                       b.rows[1][2] * src.position[2] + b.rows[1][3];
            float pz = b.rows[2][0] * src.position[0] + b.rows[2][1] * src.position[1] +
                       b.rows[2][2] * src.position[2] + b.rows[2][3];

            skinnedPos[0] += px * weight;
            skinnedPos[1] += py * weight;
            skinnedPos[2] += pz * weight;

            // Transform normal (no translation)
            float nx = b.rows[0][0] * src.normal[0] + b.rows[0][1] * src.normal[1] + b.rows[0][2] * src.normal[2];
            float ny = b.rows[1][0] * src.normal[0] + b.rows[1][1] * src.normal[1] + b.rows[1][2] * src.normal[2];
            float nz = b.rows[2][0] * src.normal[0] + b.rows[2][1] * src.normal[1] + b.rows[2][2] * src.normal[2];

            skinnedNormal[0] += nx * weight;
            skinnedNormal[1] += ny * weight;
            skinnedNormal[2] += nz * weight;
        }

        // Normalize normal
        float len = std::sqrt(skinnedNormal[0] * skinnedNormal[0] + skinnedNormal[1] * skinnedNormal[1] +
                              skinnedNormal[2] * skinnedNormal[2]);
        if (len > 0.0001f)
        {
            skinnedNormal[0] /= len;
            skinnedNormal[1] /= len;
            skinnedNormal[2] /= len;
        }

        SkinnedVertex out = {};
        std::memcpy(out.position, skinnedPos, sizeof(skinnedPos));
        std::memcpy(out.normal, skinnedNormal, sizeof(skinnedNormal));
        out.texCoord[0] = src.texCoord[0];
        out.texCoord[1] = src.texCoord[1];
        return out;
    }

} // namespace

TEST(GPUSkinning_IdentityBone_NoChange)
{
    SourceVertex src = {};
    src.position[0] = 1.0f;
    src.position[1] = 2.0f;
    src.position[2] = 3.0f;
    src.normal[0] = 0.0f;
    src.normal[1] = 1.0f;
    src.normal[2] = 0.0f;
    src.boneIndices[0] = 0;
    src.boneWeights[0] = 1.0f;

    std::vector<BoneMatrix> bones = {IdentityBone()};
    auto result = SkinVertex(src, bones);

    EXPECT_NEAR(result.position[0], 1.0f, 0.001f);
    EXPECT_NEAR(result.position[1], 2.0f, 0.001f);
    EXPECT_NEAR(result.position[2], 3.0f, 0.001f);
}

TEST(GPUSkinning_Translation_MovesVertex)
{
    SourceVertex src = {};
    src.position[0] = 0.0f;
    src.position[1] = 0.0f;
    src.position[2] = 0.0f;
    src.normal[1] = 1.0f;
    src.boneIndices[0] = 0;
    src.boneWeights[0] = 1.0f;

    std::vector<BoneMatrix> bones = {TranslationBone(5.0f, 10.0f, -3.0f)};
    auto result = SkinVertex(src, bones);

    EXPECT_NEAR(result.position[0], 5.0f, 0.001f);
    EXPECT_NEAR(result.position[1], 10.0f, 0.001f);
    EXPECT_NEAR(result.position[2], -3.0f, 0.001f);
}

TEST(GPUSkinning_Scale_ScalesVertex)
{
    SourceVertex src = {};
    src.position[0] = 1.0f;
    src.position[1] = 1.0f;
    src.position[2] = 1.0f;
    src.normal[1] = 1.0f;
    src.boneIndices[0] = 0;
    src.boneWeights[0] = 1.0f;

    std::vector<BoneMatrix> bones = {ScaleBone(2.0f)};
    auto result = SkinVertex(src, bones);

    EXPECT_NEAR(result.position[0], 2.0f, 0.001f);
    EXPECT_NEAR(result.position[1], 2.0f, 0.001f);
    EXPECT_NEAR(result.position[2], 2.0f, 0.001f);
}

TEST(GPUSkinning_TwoBoneBlend_Interpolates)
{
    SourceVertex src = {};
    src.position[0] = 0.0f;
    src.normal[1] = 1.0f;
    src.boneIndices[0] = 0;
    src.boneIndices[1] = 1;
    src.boneWeights[0] = 0.5f;
    src.boneWeights[1] = 0.5f;

    std::vector<BoneMatrix> bones = {TranslationBone(10.0f, 0.0f, 0.0f), TranslationBone(0.0f, 10.0f, 0.0f)};
    auto result = SkinVertex(src, bones);

    EXPECT_NEAR(result.position[0], 5.0f, 0.001f);
    EXPECT_NEAR(result.position[1], 5.0f, 0.001f);
}

TEST(GPUSkinning_FourBoneBlend_SumsCorrectly)
{
    SourceVertex src = {};
    src.position[0] = 0.0f;
    src.normal[1] = 1.0f;
    src.boneIndices[0] = 0;
    src.boneIndices[1] = 1;
    src.boneIndices[2] = 2;
    src.boneIndices[3] = 3;
    src.boneWeights[0] = 0.25f;
    src.boneWeights[1] = 0.25f;
    src.boneWeights[2] = 0.25f;
    src.boneWeights[3] = 0.25f;

    std::vector<BoneMatrix> bones = {TranslationBone(4.0f, 0, 0), TranslationBone(0, 4.0f, 0),
                                     TranslationBone(0, 0, 4.0f), TranslationBone(0, 0, 0)};
    auto result = SkinVertex(src, bones);

    EXPECT_NEAR(result.position[0], 1.0f, 0.001f);
    EXPECT_NEAR(result.position[1], 1.0f, 0.001f);
    EXPECT_NEAR(result.position[2], 1.0f, 0.001f);
}

TEST(GPUSkinning_ZeroWeight_Ignored)
{
    SourceVertex src = {};
    src.position[0] = 1.0f;
    src.normal[1] = 1.0f;
    src.boneIndices[0] = 0;
    src.boneIndices[1] = 1;
    src.boneWeights[0] = 1.0f;
    src.boneWeights[1] = 0.0f;

    std::vector<BoneMatrix> bones = {IdentityBone(), TranslationBone(100.0f, 0, 0)};
    auto result = SkinVertex(src, bones);

    EXPECT_NEAR(result.position[0], 1.0f, 0.001f); // bone 1 ignored
}

TEST(GPUSkinning_TexCoords_Preserved)
{
    SourceVertex src = {};
    src.texCoord[0] = 0.5f;
    src.texCoord[1] = 0.75f;
    src.normal[1] = 1.0f;
    src.boneIndices[0] = 0;
    src.boneWeights[0] = 1.0f;

    std::vector<BoneMatrix> bones = {IdentityBone()};
    auto result = SkinVertex(src, bones);

    EXPECT_NEAR(result.texCoord[0], 0.5f, 0.001f);
    EXPECT_NEAR(result.texCoord[1], 0.75f, 0.001f);
}

TEST(GPUSkinning_Normal_NormalizedAfterBlend)
{
    SourceVertex src = {};
    src.normal[0] = 1.0f;
    src.boneIndices[0] = 0;
    src.boneWeights[0] = 1.0f;

    std::vector<BoneMatrix> bones = {ScaleBone(3.0f)};
    auto result = SkinVertex(src, bones);

    float len = std::sqrt(result.normal[0] * result.normal[0] + result.normal[1] * result.normal[1] +
                          result.normal[2] * result.normal[2]);
    EXPECT_TRUE(std::abs(len - 1.0f) < 0.001f);
}

TEST(GPUSkinning_BoneMatrix_CompactLayout)
{
    // Compact bone: 3 float4 rows = 48 bytes (vs 64 for full 4x4)
    EXPECT_EQ(sizeof(BoneMatrix), 48u);
}
