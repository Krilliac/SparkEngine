/**
 * @file TestMMOAssetImport.cpp
 * @brief Real non-Windows CPU mesh import checks for Blender-authored MMO props.
 *
 * Exercises production MeshAsset::Load, not a replacement OBJ parser. Windows
 * requires a real D3D11 device and is deliberately outside this CPU test scope.
 * Material references and source/export provenance are checked separately; this
 * loader does not expose OBJ material assignments. No rendering claim is made.
 */

#include "TestFramework.h"
#include "Core/Platform.h"

#ifndef SPARK_PLATFORM_WINDOWS

#include "Graphics/AssetPipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>

namespace
{
    struct BaselineBounds
    {
        const char* filename;
        std::array<float, 3> minimum;
        std::array<float, 3> maximum;
    };

    // Source-space bounds independently extracted from the original OBJ vertices
    // at ff951c19a5e12be6ec3724696af6fe030070e8c4, before Blender replacement.
    // Do not regenerate these expectations from candidate exports/manifests.
    constexpr std::array<BaselineBounds, 16> kOriginalBounds = {{
        {"alchemy_shop.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"anvil.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"banner.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"cauldron.obj", {-0.5f, -0.4f, -0.5f}, {0.5f, 0.4f, 0.5f}},
        {"chest.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"forge.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"fountain.obj", {-1.5f, -1.0f, -1.5f}, {1.5f, 1.0f, 1.5f}},
        {"gate.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"gravestone.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"guild_hall.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"market_stall.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"pillar.obj", {-0.3f, -1.5f, -0.3f}, {0.3f, 1.5f, 0.3f}},
        {"rock_large.obj", {-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
        {"tent.obj", {-1.5f, 0.0f, -1.5f}, {1.5f, 2.0f, 1.5f}},
        {"torch.obj", {-0.1f, -0.5f, -0.1f}, {0.1f, 0.5f, 0.1f}},
        {"tree_pine.obj", {-1.0f, 0.0f, -1.0f}, {1.0f, 4.0f, 1.0f}},
    }};

    void CheckImportedModel(const BaselineBounds& baseline)
    {
        const auto path = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Assets/Models/MMO" / baseline.filename;
        ASSERT_TRUE(std::filesystem::is_regular_file(path));
        MeshAsset asset(path.string());
        ASSERT_TRUE(SUCCEEDED(asset.Load(nullptr)));
        const auto& mesh = asset.GetMeshData();
        // The existing OBJ loader can return S_OK with empty data: success alone
        // is not evidence that an asset was imported.
        ASSERT_FALSE(mesh.vertices.empty());
        ASSERT_FALSE(mesh.indices.empty());
        ASSERT_EQ(mesh.indices.size() % 3, size_t{0});
        EXPECT_TRUE(mesh.indices.size() / 3 <= 5000);

        std::array<float, 3> minimum;
        std::array<float, 3> maximum;
        minimum.fill(std::numeric_limits<float>::max());
        maximum.fill(std::numeric_limits<float>::lowest());
        for (const auto& vertex : mesh.vertices)
        {
            const std::array<float, 3> position = {vertex.position.x, vertex.position.y, vertex.position.z};
            for (size_t axis = 0; axis < 3; ++axis)
            {
                ASSERT_TRUE(std::isfinite(position[axis]));
                minimum[axis] = std::min(minimum[axis], position[axis]);
                maximum[axis] = std::max(maximum[axis], position[axis]);
            }
            ASSERT_TRUE(std::isfinite(vertex.normal.x));
            ASSERT_TRUE(std::isfinite(vertex.normal.y));
            ASSERT_TRUE(std::isfinite(vertex.normal.z));
            const double normalLengthSquared = double(vertex.normal.x) * vertex.normal.x +
                                               double(vertex.normal.y) * vertex.normal.y +
                                               double(vertex.normal.z) * vertex.normal.z;
            EXPECT_NEAR(normalLengthSquared, 1.0, 0.002);
            EXPECT_TRUE(std::isfinite(vertex.texCoord0.x));
            EXPECT_TRUE(std::isfinite(vertex.texCoord0.y));
        }
        const std::array<float, 3> reportedMinimum = {mesh.boundingBoxMin.x, mesh.boundingBoxMin.y,
                                                      mesh.boundingBoxMin.z};
        const std::array<float, 3> reportedMaximum = {mesh.boundingBoxMax.x, mesh.boundingBoxMax.y,
                                                      mesh.boundingBoxMax.z};
        for (size_t axis = 0; axis < 3; ++axis)
        {
            EXPECT_NEAR(minimum[axis], baseline.minimum[axis], 0.0001f);
            EXPECT_NEAR(maximum[axis], baseline.maximum[axis], 0.0001f);
            EXPECT_NEAR(reportedMinimum[axis], minimum[axis], 0.0001f);
            EXPECT_NEAR(reportedMaximum[axis], maximum[axis], 0.0001f);
        }
        for (size_t index = 0; index < mesh.indices.size(); index += 3)
        {
            for (size_t corner = 0; corner < 3; ++corner)
                ASSERT_TRUE(mesh.indices[index + corner] < mesh.vertices.size());
            const auto& a = mesh.vertices[mesh.indices[index]].position;
            const auto& b = mesh.vertices[mesh.indices[index + 1]].position;
            const auto& c = mesh.vertices[mesh.indices[index + 2]].position;
            const double ux = double(b.x) - a.x, uy = double(b.y) - a.y, uz = double(b.z) - a.z;
            const double vx = double(c.x) - a.x, vy = double(c.y) - a.y, vz = double(c.z) - a.z;
            const double cx = uy * vz - uz * vy, cy = uz * vx - ux * vz, cz = ux * vy - uy * vx;
            EXPECT_TRUE(cx * cx + cy * cy + cz * cz > 1e-20);
        }
    }
} // namespace

TEST(MMOAssetImport_alchemy_shop_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[0]);
}

TEST(MMOAssetImport_anvil_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[1]);
}

TEST(MMOAssetImport_banner_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[2]);
}

TEST(MMOAssetImport_cauldron_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[3]);
}

TEST(MMOAssetImport_chest_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[4]);
}

TEST(MMOAssetImport_forge_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[5]);
}

TEST(MMOAssetImport_fountain_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[6]);
}

TEST(MMOAssetImport_gate_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[7]);
}

TEST(MMOAssetImport_gravestone_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[8]);
}

TEST(MMOAssetImport_guild_hall_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[9]);
}

TEST(MMOAssetImport_market_stall_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[10]);
}

TEST(MMOAssetImport_pillar_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[11]);
}

TEST(MMOAssetImport_rock_large_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[12]);
}

TEST(MMOAssetImport_tent_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[13]);
}

TEST(MMOAssetImport_torch_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[14]);
}

TEST(MMOAssetImport_tree_pine_RealCPUContract)
{
    CheckImportedModel(kOriginalBounds[15]);
}

#endif // !SPARK_PLATFORM_WINDOWS
