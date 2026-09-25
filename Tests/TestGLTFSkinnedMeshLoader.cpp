/**
 * @file TestGLTFSkinnedMeshLoader.cpp
 * @brief CPU-only tests for the fail-closed glTF 2.0 skin import (JOINTS_0/WEIGHTS_0, inverse binds, joint tree).
 *
 * Every fixture is a GLB assembled in-test. Expected values are written out by hand from the
 * fixture's authored transforms rather than recomputed with the loader's own math.
 */

#include "TestFramework.h"
#include "Graphics/GLTFSkinnedMeshLoader.h"
#include "Graphics/GLTFStaticMeshLoader.h"
#include "Graphics/GPUSkinning.h"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace
{
    using Spark::Graphics::Detail::GLTFSkinnedMeshData;
    using Spark::Graphics::Detail::LoadGLTFSkinnedMesh;
    using Matrix = std::array<float, 16>;

    constexpr uint32_t kUnsignedByte = 5121;
    constexpr uint32_t kUnsignedShort = 5123;
    constexpr uint32_t kFloat = 5126;

    Matrix Translation(float x, float y, float z)
    {
        // glTF column-major: translation occupies elements 12..14.
        return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, x, y, z, 1};
    }

    // Armature (non-joint, +2 Z) -> Root joint (+1 Y) -> Tip joint (+2 Y); Body binds the mesh.
    // Skin joints are listed Tip-first so the loader has to reorder them parent-first.
    const std::string kDefaultNodes = "[{\"name\":\"Armature\",\"translation\":[0,0,2],\"children\":[1,3]},"
                                      "{\"name\":\"Root\",\"translation\":[0,1,0],\"children\":[2]},"
                                      "{\"name\":\"Tip\",\"translation\":[0,2,0]},"
                                      "{\"name\":\"Body\",\"mesh\":0,\"skin\":0}]";

    struct SkinFixture
    {
        std::vector<std::array<uint32_t, 4>> joints = {{0, 1, 0, 0}, {1, 0, 0, 0}, {0, 1, 0, 0}};
        std::vector<std::array<float, 4>> weights = {
            {0.25f, 0.75f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 0.0f}, {0.502f, 0.502f, 0.0f, 0.0f}};
        // Global binds: Tip at (0,3,2), Root at (0,1,2); inverse binds undo them.
        std::vector<Matrix> inverseBinds = {Translation(0, -3, -2), Translation(0, -1, -2)};
        uint32_t jointComponentType = kUnsignedByte;
        std::string nodes = kDefaultNodes;
        std::string skinJoints = "[2,1]";
        std::string extraAttributes;
    };

    void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            bytes.push_back(static_cast<uint8_t>(value >> shift));
        }
    }

    void AppendFloat(std::vector<uint8_t>& bytes, float value)
    {
        AppendU32(bytes, std::bit_cast<uint32_t>(value));
    }

    void PadToFour(std::vector<uint8_t>& bytes)
    {
        while (bytes.size() % 4 != 0)
        {
            bytes.push_back(0);
        }
    }

    std::string View(size_t offset, size_t length)
    {
        return "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(length) +
               "}";
    }

    std::string Accessor(int view, uint32_t componentType, size_t count, const char* type, bool normalized = false)
    {
        return "{\"bufferView\":" + std::to_string(view) + ",\"componentType\":" + std::to_string(componentType) +
               ",\"count\":" + std::to_string(count) + ",\"type\":\"" + type + "\"" +
               (normalized ? ",\"normalized\":true" : "") + "}";
    }

    std::vector<uint8_t> BuildGLB(const SkinFixture& fixture)
    {
        std::vector<uint8_t> bin;
        std::vector<std::string> views;
        auto beginView = [&bin]() { return bin.size(); };
        auto endView = [&bin, &views](size_t start)
        {
            views.push_back(View(start, bin.size() - start));
            PadToFour(bin);
        };

        size_t start = beginView();
        for (float value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f})
        {
            AppendFloat(bin, value);
        }
        endView(start);

        start = beginView();
        for (int vertex = 0; vertex < 3; ++vertex)
        {
            for (float value : {0.0f, 0.0f, 1.0f})
            {
                AppendFloat(bin, value);
            }
        }
        endView(start);

        start = beginView();
        for (const auto& influence : fixture.joints)
        {
            for (uint32_t joint : influence)
            {
                if (fixture.jointComponentType == kUnsignedByte)
                {
                    bin.push_back(static_cast<uint8_t>(joint));
                }
                else if (fixture.jointComponentType == kUnsignedShort)
                {
                    bin.push_back(static_cast<uint8_t>(joint));
                    bin.push_back(static_cast<uint8_t>(joint >> 8));
                }
                else
                {
                    AppendFloat(bin, static_cast<float>(joint));
                }
            }
        }
        endView(start);

        start = beginView();
        for (const auto& influence : fixture.weights)
        {
            for (float weight : influence)
            {
                AppendFloat(bin, weight);
            }
        }
        endView(start);

        start = beginView();
        for (uint8_t index : {0, 0, 1, 0, 2, 0})
        {
            bin.push_back(index);
        }
        endView(start);

        if (!fixture.inverseBinds.empty())
        {
            start = beginView();
            for (const Matrix& matrix : fixture.inverseBinds)
            {
                for (float value : matrix)
                {
                    AppendFloat(bin, value);
                }
            }
            endView(start);
        }

        std::string json =
            "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"byteLength\":" + std::to_string(bin.size()) +
            "}],\"bufferViews\":[";
        for (size_t i = 0; i < views.size(); ++i)
        {
            json += (i ? "," : "") + views[i];
        }
        json += "],\"accessors\":[" + Accessor(0, kFloat, 3, "VEC3") + "," + Accessor(1, kFloat, 3, "VEC3") + "," +
                Accessor(2, fixture.jointComponentType, 3, "VEC4") + "," + Accessor(3, kFloat, 3, "VEC4") + "," +
                Accessor(4, kUnsignedShort, 3, "SCALAR");
        if (!fixture.inverseBinds.empty())
        {
            json += "," + Accessor(5, kFloat, fixture.inverseBinds.size(), "MAT4");
        }
        json += "],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"JOINTS_0\":2,"
                "\"WEIGHTS_0\":3" +
                fixture.extraAttributes + "},\"indices\":4}]}],\"nodes\":" + fixture.nodes +
                ",\"skins\":[{\"name\":\"Rig\",\"joints\":" + fixture.skinJoints +
                (fixture.inverseBinds.empty() ? std::string() : ",\"inverseBindMatrices\":5") + "}]}";

        while (json.size() % 4 != 0)
        {
            json.push_back(' ');
        }
        std::vector<uint8_t> glb;
        AppendU32(glb, 0x46546C67);
        AppendU32(glb, 2);
        AppendU32(glb, static_cast<uint32_t>(12 + 8 + json.size() + 8 + bin.size()));
        AppendU32(glb, static_cast<uint32_t>(json.size()));
        AppendU32(glb, 0x4E4F534A);
        glb.insert(glb.end(), json.begin(), json.end());
        AppendU32(glb, static_cast<uint32_t>(bin.size()));
        AppendU32(glb, 0x004E4942);
        glb.insert(glb.end(), bin.begin(), bin.end());
        return glb;
    }

    struct TemporaryGLB
    {
        explicit TemporaryGLB(const std::string& name, const SkinFixture& fixture)
            : directory(std::filesystem::temp_directory_path() / ("spark_gltf_skin_" + name)),
              path(directory / "skin.glb")
        {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
            std::filesystem::create_directories(directory, ec);
            const std::vector<uint8_t> glb = BuildGLB(fixture);
            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            file.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
        }

        ~TemporaryGLB()
        {
            std::error_code ec;
            std::filesystem::remove_all(directory, ec);
        }

        std::filesystem::path directory;
        std::filesystem::path path;
    };

    /// Load a fixture expected to fail; returns the diagnostic and checks the output was cleared.
    std::string LoadRejected(const std::string& name, const SkinFixture& fixture, bool& cleared)
    {
        TemporaryGLB glb(name, fixture);
        GLTFSkinnedMeshData meshData;
        meshData.vertices.resize(1);
        std::string error;
        const bool loaded = LoadGLTFSkinnedMesh(glb.path, meshData, error);
        cleared = !loaded && meshData.vertices.empty() && meshData.indices.empty() && meshData.skeleton.bones.empty();
        return loaded ? std::string("<loaded>") : error;
    }

    /// Chain of @p jointCount joints under a mesh node; used for palette-size boundaries.
    SkinFixture MakeJointChain(size_t jointCount)
    {
        SkinFixture fixture;
        fixture.inverseBinds.clear();
        fixture.weights = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}};
        fixture.joints = {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}};
        fixture.nodes = "[";
        fixture.skinJoints = "[";
        for (size_t i = 0; i < jointCount; ++i)
        {
            fixture.nodes += "{\"name\":\"J" + std::to_string(i) + "\"" +
                             (i + 1 < jointCount ? ",\"children\":[" + std::to_string(i + 1) + "]" : "") + "},";
            fixture.skinJoints += (i ? "," : "") + std::to_string(i);
        }
        fixture.nodes += "{\"mesh\":0,\"skin\":0}]";
        fixture.skinJoints += "]";
        return fixture;
    }
} // namespace

TEST(GLTF_Skinning_LoadsTwoJointSkinWithOrderedSkeleton)
{
    TemporaryGLB glb("two_joint", SkinFixture{});
    GLTFSkinnedMeshData meshData;
    std::string error;
    ASSERT_TRUE(LoadGLTFSkinnedMesh(glb.path, meshData, error));

    const auto& skeleton = meshData.skeleton;
    EXPECT_EQ(skeleton.name, std::string("Rig"));
    ASSERT_EQ(skeleton.bones.size(), 2u);
    EXPECT_EQ(skeleton.bones[0].name, std::string("Root"));
    EXPECT_EQ(skeleton.bones[0].parentIndex, -1);
    EXPECT_EQ(skeleton.bones[1].name, std::string("Tip"));
    EXPECT_EQ(skeleton.bones[1].parentIndex, 0);
    EXPECT_EQ(skeleton.FindBone("Tip"), 1);

    // Root's bind pose folds in the non-joint Armature: (0,1,0) + (0,0,2).
    const auto& root = skeleton.bones[0];
    EXPECT_NEAR(root.localBindPose._41, 0.0f, 1e-6f);
    EXPECT_NEAR(root.localBindPose._42, 1.0f, 1e-6f);
    EXPECT_NEAR(root.localBindPose._43, 2.0f, 1e-6f);
    EXPECT_NEAR(root.localBindPose._11, 1.0f, 1e-6f);
    EXPECT_NEAR(root.localBindPose._44, 1.0f, 1e-6f);
    EXPECT_NEAR(root.offsetMatrix._42, -1.0f, 1e-6f);
    EXPECT_NEAR(root.offsetMatrix._43, -2.0f, 1e-6f);

    const auto& tip = skeleton.bones[1];
    EXPECT_NEAR(tip.localBindPose._42, 2.0f, 1e-6f);
    EXPECT_NEAR(tip.localBindPose._43, 0.0f, 1e-6f);
    EXPECT_NEAR(tip.offsetMatrix._42, -3.0f, 1e-6f);
    EXPECT_NEAR(tip.offsetMatrix._43, -2.0f, 1e-6f);
    // Bind pose composed down the chain lands exactly where the inverse bind undoes it.
    EXPECT_NEAR(tip.localBindPose._42 + root.localBindPose._42 + tip.offsetMatrix._42, 0.0f, 1e-6f);
    EXPECT_NEAR(tip.localBindPose._43 + root.localBindPose._43 + tip.offsetMatrix._43, 0.0f, 1e-6f);

    ASSERT_EQ(meshData.vertices.size(), 3u);
    ASSERT_EQ(meshData.indices.size(), 3u);
    ASSERT_EQ(meshData.primitives.size(), 1u);
    EXPECT_EQ(meshData.indices[2], 2u);
    EXPECT_NEAR(meshData.vertices[1].position[0], 1.0f, 1e-6f);
    EXPECT_NEAR(meshData.vertices[0].normal[2], 1.0f, 1e-6f);

    // Skin joint 0 is Tip (bone 1) and skin joint 1 is Root (bone 0).
    const auto& v0 = meshData.vertices[0];
    EXPECT_EQ(v0.joints[0], 1u);
    EXPECT_EQ(v0.joints[1], 0u);
    EXPECT_NEAR(v0.weights[0], 0.25f, 1e-6f);
    EXPECT_NEAR(v0.weights[1], 0.75f, 1e-6f);
    EXPECT_EQ(meshData.vertices[1].joints[0], 0u);
    EXPECT_NEAR(meshData.vertices[1].weights[0], 1.0f, 1e-6f);

    // 0.502 + 0.502 is within tolerance and renormalizes to an exact half/half split.
    const auto& v2 = meshData.vertices[2];
    EXPECT_NEAR(v2.weights[0], 0.5f, 1e-6f);
    EXPECT_NEAR(v2.weights[1], 0.5f, 1e-6f);
    EXPECT_NEAR(v2.weights[2], 0.0f, 1e-6f);
}

TEST(GLTF_Skinning_UnsignedShortJointsWithoutInverseBindsUseIdentity)
{
    SkinFixture fixture;
    fixture.jointComponentType = kUnsignedShort;
    fixture.inverseBinds.clear();
    TemporaryGLB glb("ushort", fixture);
    GLTFSkinnedMeshData meshData;
    std::string error;
    ASSERT_TRUE(LoadGLTFSkinnedMesh(glb.path, meshData, error));
    ASSERT_EQ(meshData.skeleton.bones.size(), 2u);
    for (const auto& bone : meshData.skeleton.bones)
    {
        EXPECT_NEAR(bone.offsetMatrix._11, 1.0f, 1e-6f);
        EXPECT_NEAR(bone.offsetMatrix._42, 0.0f, 1e-6f);
        EXPECT_NEAR(bone.offsetMatrix._44, 1.0f, 1e-6f);
    }
    EXPECT_EQ(meshData.vertices[0].joints[0], 1u);
}

TEST(GLTF_Skinning_AcceptsGPUPaletteSizedSkin)
{
    TemporaryGLB glb("max_joints", MakeJointChain(Spark::Graphics::kMaxBonesPerMesh));
    GLTFSkinnedMeshData meshData;
    std::string error;
    ASSERT_TRUE(LoadGLTFSkinnedMesh(glb.path, meshData, error));
    ASSERT_EQ(meshData.skeleton.bones.size(), 256u);
    EXPECT_EQ(meshData.skeleton.bones[255].parentIndex, 254);
    EXPECT_EQ(meshData.skeleton.bones[255].name, std::string("J255"));
}

TEST(GLTF_Skinning_RejectsSkinLargerThanGPUPalette)
{
    bool cleared = false;
    const std::string error = LoadRejected("too_many_joints", MakeJointChain(257), cleared);
    EXPECT_STR_CONTAINS(error, "257 joints");
    EXPECT_STR_CONTAINS(error, "at most 256");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsSecondInfluenceSet)
{
    SkinFixture fixture;
    fixture.extraAttributes = ",\"JOINTS_1\":2,\"WEIGHTS_1\":3";
    bool cleared = false;
    const std::string error = LoadRejected("joints1", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "_1");
    EXPECT_STR_CONTAINS(error, "4 influences");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsJointIndexOutsideSkin)
{
    SkinFixture fixture;
    fixture.joints[1] = {2, 0, 0, 0};
    bool cleared = false;
    const std::string error = LoadRejected("joint_range", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "vertex 1");
    EXPECT_STR_CONTAINS(error, "references joint 2 but the skin has 2 joints");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsNegativeWeights)
{
    SkinFixture fixture;
    fixture.weights[0] = {1.25f, -0.25f, 0.0f, 0.0f};
    bool cleared = false;
    const std::string error = LoadRejected("negative_weight", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "negative weight");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsNonFiniteWeights)
{
    SkinFixture fixture;
    fixture.weights[2] = {std::numeric_limits<float>::quiet_NaN(), 1.0f, 0.0f, 0.0f};
    bool cleared = false;
    const std::string error = LoadRejected("nan_weight", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "finite");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsZeroSumWeights)
{
    SkinFixture fixture;
    fixture.weights[1] = {0.0f, 0.0f, 0.0f, 0.0f};
    bool cleared = false;
    const std::string error = LoadRejected("zero_weight", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "vertex 1 has zero-sum weights");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsWeightSumOutsideTolerance)
{
    SkinFixture fixture;
    fixture.weights[0] = {0.25f, 0.25f, 0.0f, 0.0f};
    bool cleared = false;
    const std::string error = LoadRejected("weight_sum", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "summing to 0.5");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsFloatJointComponentType)
{
    SkinFixture fixture;
    fixture.jointComponentType = kFloat;
    bool cleared = false;
    const std::string error = LoadRejected("float_joints", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "JOINTS_0 must be");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsCyclicNodeHierarchy)
{
    SkinFixture fixture;
    fixture.nodes = "[{\"name\":\"A\",\"children\":[1]},{\"name\":\"B\",\"children\":[0]},{\"mesh\":0,\"skin\":0}]";
    fixture.skinJoints = "[0,1]";
    bool cleared = false;
    const std::string error = LoadRejected("cycle", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "cycle");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsDisconnectedJoints)
{
    SkinFixture fixture;
    fixture.nodes = "[{\"name\":\"A\"},{\"name\":\"B\"},{\"mesh\":0,\"skin\":0}]";
    fixture.skinJoints = "[0,1]";
    bool cleared = false;
    const std::string error = LoadRejected("disconnected", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "2 disconnected trees");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsJointSeparatedByNonJointNode)
{
    SkinFixture fixture;
    fixture.nodes = "[{\"name\":\"Root\",\"children\":[1]},{\"name\":\"Offset\",\"children\":[2]},"
                    "{\"name\":\"Tip\"},{\"mesh\":0,\"skin\":0}]";
    fixture.skinJoints = "[2,0]";
    bool cleared = false;
    const std::string error = LoadRejected("gap", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "separated from its ancestor joint node 0 'Root' by non-joint node 1 'Offset'");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsDuplicateJoints)
{
    SkinFixture fixture;
    fixture.skinJoints = "[1,1]";
    bool cleared = false;
    const std::string error = LoadRejected("duplicate", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "more than once");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsSingularInverseBind)
{
    SkinFixture fixture;
    fixture.inverseBinds[1] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    bool cleared = false;
    const std::string error = LoadRejected("singular", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "inverse bind matrix of joint 1 (node 1 'Root') is singular");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsNonAffineInverseBind)
{
    SkinFixture fixture;
    fixture.inverseBinds[0][3] = 0.5f;
    bool cleared = false;
    const std::string error = LoadRejected("projective", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "not affine");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_RejectsMeshNodeWithoutSkin)
{
    SkinFixture fixture;
    fixture.nodes = "[{\"name\":\"Armature\",\"children\":[1,3]},{\"name\":\"Root\",\"children\":[2]},"
                    "{\"name\":\"Tip\"},{\"name\":\"Prop\",\"mesh\":0}]";
    bool cleared = false;
    const std::string error = LoadRejected("unskinned_node", fixture, cleared);
    EXPECT_STR_CONTAINS(error, "node 3 'Prop' instances a mesh without the skin");
    EXPECT_TRUE(cleared);
}

TEST(GLTF_Skinning_StaticLoaderStillRejectsSkinnedFile)
{
    TemporaryGLB glb("static_rejects", SkinFixture{});
    Spark::Graphics::Detail::GLTFStaticMeshData meshData;
    std::string error;
    EXPECT_FALSE(Spark::Graphics::Detail::LoadGLTFStaticMesh(glb.path, meshData, error));
    EXPECT_STR_CONTAINS(error, "skins and animations");
}
