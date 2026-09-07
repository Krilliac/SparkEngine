/**
 * @file TestGLTFStaticMeshLoader.cpp
 * @brief CPU-only tests for the fail-closed glTF 2.0 static-mesh subset.
 */

#include "TestFramework.h"
#include "Graphics/GLTFStaticMeshLoader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    using Spark::Graphics::Detail::GLTFStaticMeshData;

    struct TemporaryDirectory
    {
        explicit TemporaryDirectory(const char* name) : path(std::filesystem::temp_directory_path() / name)
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
            std::filesystem::create_directories(path, ec);
        }

        ~TemporaryDirectory()
        {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }

        std::filesystem::path path;
    };

    void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
        bytes.push_back(static_cast<uint8_t>(value >> 16));
        bytes.push_back(static_cast<uint8_t>(value >> 24));
    }

    void AppendU16(std::vector<uint8_t>& bytes, uint16_t value)
    {
        bytes.push_back(static_cast<uint8_t>(value));
        bytes.push_back(static_cast<uint8_t>(value >> 8));
    }

    void AppendFloat(std::vector<uint8_t>& bytes, float value)
    {
        AppendU32(bytes, std::bit_cast<uint32_t>(value));
    }

    std::vector<uint8_t> MakeTriangleBuffer()
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(102);
        for (float value : {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f})
        {
            AppendFloat(bytes, value);
        }
        for (int vertex = 0; vertex < 3; ++vertex)
        {
            AppendFloat(bytes, 0.0f);
            AppendFloat(bytes, 0.0f);
            AppendFloat(bytes, 1.0f);
        }
        for (float value : {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f})
        {
            AppendFloat(bytes, value);
        }
        AppendU16(bytes, 0);
        AppendU16(bytes, 1);
        AppendU16(bytes, 2);
        return bytes;
    }

    std::string MakeTriangleJson(const std::string& bufferFields)
    {
        return "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{" + bufferFields +
               "}],\"bufferViews\":["
               "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
               "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":36},"
               "{\"buffer\":0,\"byteOffset\":72,\"byteLength\":24},"
               "{\"buffer\":0,\"byteOffset\":96,\"byteLength\":6}],"
               "\"accessors\":["
               "{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
               "{\"bufferView\":1,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"},"
               "{\"bufferView\":2,\"componentType\":5126,\"count\":3,\"type\":\"VEC2\"},"
               "{\"bufferView\":3,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}],"
               "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
               "\"TEXCOORD_0\":2},\"indices\":3}]}]}";
    }

    void ReplaceOnce(std::string& text, const std::string& from, const std::string& to)
    {
        const size_t position = text.find(from);
        if (position != std::string::npos)
        {
            text.replace(position, from.size(), to);
        }
    }

    void WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    std::vector<uint8_t> MakeGLB(std::string json, std::vector<uint8_t> binary)
    {
        while (json.size() % 4 != 0)
        {
            json.push_back(' ');
        }
        while (binary.size() % 4 != 0)
        {
            binary.push_back(0);
        }

        std::vector<uint8_t> glb;
        const uint32_t totalLength = static_cast<uint32_t>(12 + 8 + json.size() + 8 + binary.size());
        AppendU32(glb, 0x46546C67);
        AppendU32(glb, 2);
        AppendU32(glb, totalLength);
        AppendU32(glb, static_cast<uint32_t>(json.size()));
        AppendU32(glb, 0x4E4F534A);
        glb.insert(glb.end(), json.begin(), json.end());
        AppendU32(glb, static_cast<uint32_t>(binary.size()));
        AppendU32(glb, 0x004E4942);
        glb.insert(glb.end(), binary.begin(), binary.end());
        return glb;
    }

    bool LoadExternalTriangle(const std::filesystem::path& root, std::string json, GLTFStaticMeshData& meshData,
                              std::string& error)
    {
        WriteBytes(root / "triangle.bin", MakeTriangleBuffer());
        WriteText(root / "triangle.gltf", json);
        return Spark::Graphics::Detail::LoadGLTFStaticMesh(root / "triangle.gltf", meshData, error);
    }
} // namespace

TEST(GLTFStaticMesh_LoadsMinimalExternalBufferGLTF)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_external");
    GLTFStaticMeshData meshData;
    std::string error;
    ASSERT_TRUE(LoadExternalTriangle(temp.path, MakeTriangleJson("\"byteLength\":102,\"uri\":\"triangle.bin\""),
                                     meshData, error));
    EXPECT_EQ(meshData.vertices.size(), 3u);
    EXPECT_EQ(meshData.indices.size(), 3u);
    EXPECT_EQ(meshData.primitives.size(), 1u);
    EXPECT_EQ(meshData.indices[0], 0u);
    EXPECT_EQ(meshData.indices[1], 1u);
    EXPECT_EQ(meshData.indices[2], 2u);
    EXPECT_NEAR(meshData.vertices[1].position[0], 1.0f, 0.0001f);
    EXPECT_NEAR(meshData.vertices[2].texCoord[1], 1.0f, 0.0001f);
    EXPECT_NEAR(meshData.vertices[0].normal[2], 1.0f, 0.0001f);
}

TEST(GLTFStaticMesh_LoadsMinimalGLB)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_glb");
    WriteBytes(temp.path / "triangle.glb", MakeGLB(MakeTriangleJson("\"byteLength\":102"), MakeTriangleBuffer()));

    GLTFStaticMeshData meshData;
    std::string error;
    ASSERT_TRUE(Spark::Graphics::Detail::LoadGLTFStaticMesh(temp.path / "triangle.glb", meshData, error));
    EXPECT_EQ(meshData.vertices.size(), 3u);
    EXPECT_EQ(meshData.indices.size(), 3u);
    EXPECT_EQ(meshData.primitives[0].indexStart, 0u);
    EXPECT_EQ(meshData.primitives[0].indexCount, 3u);
}

TEST(GLTFStaticMesh_GeneratesNormalsWhenAttributeIsMissing)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_normals");
    std::string json = MakeTriangleJson("\"byteLength\":102,\"uri\":\"triangle.bin\"");
    ReplaceOnce(json, "\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2", "\"POSITION\":0,\"TEXCOORD_0\":2");

    GLTFStaticMeshData meshData;
    std::string error;
    ASSERT_TRUE(LoadExternalTriangle(temp.path, json, meshData, error));
    EXPECT_NEAR(meshData.vertices[0].normal[0], 0.0f, 0.0001f);
    EXPECT_NEAR(meshData.vertices[0].normal[1], 0.0f, 0.0001f);
    EXPECT_NEAR(meshData.vertices[0].normal[2], 1.0f, 0.0001f);
}

TEST(GLTFStaticMesh_RejectsZeroCountBeforeCgltfValidation)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_zero_count");
    std::string json = MakeTriangleJson("\"byteLength\":102,\"uri\":\"triangle.bin\"");
    ReplaceOnce(json, "\"count\":3,\"type\":\"VEC3\"", "\"count\":0,\"type\":\"VEC3\"");

    GLTFStaticMeshData meshData;
    std::string error;
    EXPECT_FALSE(LoadExternalTriangle(temp.path, json, meshData, error));
    EXPECT_TRUE(error.find("zero-count") != std::string::npos);
    EXPECT_TRUE(meshData.vertices.empty());
}

TEST(GLTFStaticMesh_RejectsSparseAccessorsBeforeCgltfValidation)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_sparse");
    std::string json = MakeTriangleJson("\"byteLength\":102,\"uri\":\"triangle.bin\"");
    ReplaceOnce(json, "\"count\":3,\"type\":\"VEC3\"}",
                "\"count\":3,\"type\":\"VEC3\",\"sparse\":{\"count\":1,\"indices\":{\"bufferView\":3,"
                "\"componentType\":5123},\"values\":{\"bufferView\":0}}}");

    GLTFStaticMeshData meshData;
    std::string error;
    EXPECT_FALSE(LoadExternalTriangle(temp.path, json, meshData, error));
    EXPECT_TRUE(error.find("sparse") != std::string::npos);
    EXPECT_TRUE(meshData.indices.empty());
}

TEST(GLTFStaticMesh_RejectsOverflowingAccessorRangeBeforeCgltfValidation)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_bounds");
    std::string json = MakeTriangleJson("\"byteLength\":102,\"uri\":\"triangle.bin\"");
    ReplaceOnce(json, "\"bufferView\":0,\"componentType\":5126",
                "\"bufferView\":0,\"byteOffset\":32,\"componentType\":5126");

    GLTFStaticMeshData meshData;
    std::string error;
    EXPECT_FALSE(LoadExternalTriangle(temp.path, json, meshData, error));
    EXPECT_TRUE(error.find("exceeds") != std::string::npos);
    EXPECT_TRUE(meshData.vertices.empty());
}

TEST(GLTFStaticMesh_RejectsRequiredExtensions)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_extensions");
    std::string json = MakeTriangleJson("\"byteLength\":102,\"uri\":\"triangle.bin\"");
    ReplaceOnce(json, "\"asset\":{\"version\":\"2.0\"}",
                "\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"KHR_draco_mesh_compression\"]");

    GLTFStaticMeshData meshData;
    std::string error;
    EXPECT_FALSE(LoadExternalTriangle(temp.path, json, meshData, error));
    EXPECT_TRUE(error.find("required glTF extensions") != std::string::npos);
}

TEST(GLTFStaticMesh_RejectsSkinsAndAnimations)
{
    TemporaryDirectory temp("spark_gltf_static_mesh_skin_animation");
    std::string json = MakeTriangleJson("\"byteLength\":102,\"uri\":\"triangle.bin\"");
    ReplaceOnce(json, "\"meshes\":[",
                "\"nodes\":[{}],\"skins\":[{\"joints\":[0]}],\"animations\":[{\"channels\":[],"
                "\"samplers\":[]}],\"meshes\":[");

    GLTFStaticMeshData meshData;
    std::string error;
    EXPECT_FALSE(LoadExternalTriangle(temp.path, json, meshData, error));
    EXPECT_TRUE(error.find("skins and animations") != std::string::npos);
}

TEST(GLTFStaticMesh_LoadsBlenderAuthoredStaticBox)
{
    // Authored Z-up bounds [0,2]x[0,4]x[0,6] export as glTF (x,z,-y).
    // This exercises the real Blender export, not a hand-written glTF buffer.
    const auto path =
        std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Tests/Fixtures/GLTFStaticMesh/BlenderBox/authored_box.glb";
    GLTFStaticMeshData meshData;
    std::string error;
    ASSERT_TRUE(Spark::Graphics::Detail::LoadGLTFStaticMesh(path, meshData, error));
    ASSERT_EQ(meshData.vertices.size(), 24u);
    ASSERT_EQ(meshData.indices.size(), 36u);
    ASSERT_EQ(meshData.primitives.size(), 1u);
    EXPECT_EQ(meshData.primitives[0].indexStart, 0u);
    EXPECT_EQ(meshData.primitives[0].indexCount, 36u);

    constexpr float tolerance = 0.0001f;
    constexpr std::array<float, 3> expectedMin = {0.0f, 0.0f, -4.0f};
    constexpr std::array<float, 3> expectedMax = {2.0f, 6.0f, 0.0f};
    constexpr std::array<std::array<float, 3>, 8> corners = {
        {{0, 0, 0}, {2, 0, 0}, {2, 0, -4}, {0, 0, -4}, {0, 6, 0}, {2, 6, 0}, {2, 6, -4}, {0, 6, -4}}};
    // Original author.py loop order for faces -X,+X,-Y,+Y,-Z,+Z after export.
    constexpr std::array<std::array<size_t, 4>, 6> faceCorners = {
        {{3, 0, 4, 7}, {1, 2, 6, 5}, {0, 3, 2, 1}, {4, 5, 6, 7}, {2, 3, 7, 6}, {0, 1, 5, 4}}};
    // Blender's V coordinate is flipped by glTF export; asymmetry exposes flips.
    constexpr std::array<std::array<float, 2>, 4> expectedUV = {
        {{0.125f, 0.75f}, {0.875f, 0.75f}, {0.875f, 0.375f}, {0.125f, 0.375f}}};
    std::array<std::array<int, 4>, 6> faceCornerCounts{};
    auto minimum = meshData.vertices[0].position;
    auto maximum = minimum;
    for (const auto& vertex : meshData.vertices)
    {
        size_t normalAxis = 3;
        float normalLengthSquared = 0.0f;
        for (size_t axis = 0; axis < 3; ++axis)
        {
            minimum[axis] = std::min(minimum[axis], vertex.position[axis]);
            maximum[axis] = std::max(maximum[axis], vertex.position[axis]);
            normalLengthSquared += vertex.normal[axis] * vertex.normal[axis];
            if (std::abs(vertex.normal[axis]) > 0.5f)
            {
                normalAxis = axis;
            }
        }
        EXPECT_NEAR(normalLengthSquared, 1.0f, tolerance);
        ASSERT_TRUE(normalAxis < 3);
        const size_t face = normalAxis * 2 + (vertex.normal[normalAxis] > 0.0f ? 1 : 0);
        for (size_t axis = 0; axis < 3; ++axis)
        {
            const float expectedNormal = axis == normalAxis ? (face % 2 == 0 ? -1.0f : 1.0f) : 0.0f;
            EXPECT_NEAR(vertex.normal[axis], expectedNormal, tolerance);
        }
        size_t matchedCorner = 4;
        for (size_t corner = 0; corner < 4; ++corner)
        {
            const auto& expectedPosition = corners[faceCorners[face][corner]];
            if (std::abs(vertex.position[0] - expectedPosition[0]) < tolerance &&
                std::abs(vertex.position[1] - expectedPosition[1]) < tolerance &&
                std::abs(vertex.position[2] - expectedPosition[2]) < tolerance)
            {
                matchedCorner = corner;
                break;
            }
        }
        ASSERT_TRUE(matchedCorner < 4);
        ++faceCornerCounts[face][matchedCorner];
        EXPECT_NEAR(vertex.texCoord[0], expectedUV[matchedCorner][0], tolerance);
        EXPECT_NEAR(vertex.texCoord[1], expectedUV[matchedCorner][1], tolerance);
    }
    for (size_t axis = 0; axis < 3; ++axis)
    {
        EXPECT_NEAR(minimum[axis], expectedMin[axis], tolerance);
        EXPECT_NEAR(maximum[axis], expectedMax[axis], tolerance);
    }
    for (const auto& face : faceCornerCounts)
    {
        for (int count : face)
        {
            EXPECT_EQ(count, 1);
        }
    }

    std::array<bool, 24> referenced{};
    for (size_t triangle = 0; triangle < meshData.indices.size(); triangle += 3)
    {
        for (size_t offset = 0; offset < 3; ++offset)
        {
            ASSERT_TRUE(meshData.indices[triangle + offset] < meshData.vertices.size());
            referenced[meshData.indices[triangle + offset]] = true;
        }
        const auto& a = meshData.vertices[meshData.indices[triangle]];
        const auto& b = meshData.vertices[meshData.indices[triangle + 1]];
        const auto& c = meshData.vertices[meshData.indices[triangle + 2]];
        std::array<float, 3> ab{}, ac{};
        for (size_t axis = 0; axis < 3; ++axis)
        {
            ab[axis] = b.position[axis] - a.position[axis];
            ac[axis] = c.position[axis] - a.position[axis];
            EXPECT_NEAR(a.normal[axis], b.normal[axis], tolerance);
            EXPECT_NEAR(a.normal[axis], c.normal[axis], tolerance);
        }
        const std::array<float, 3> cross = {ab[1] * ac[2] - ab[2] * ac[1], ab[2] * ac[0] - ab[0] * ac[2],
                                            ab[0] * ac[1] - ab[1] * ac[0]};
        const float orientedArea = cross[0] * a.normal[0] + cross[1] * a.normal[1] + cross[2] * a.normal[2];
        const float expectedArea = std::abs(a.normal[0]) > 0.5f ? 24.0f : std::abs(a.normal[1]) > 0.5f ? 8.0f : 12.0f;
        EXPECT_NEAR(orientedArea, expectedArea, tolerance);
    }
    for (bool used : referenced)
    {
        EXPECT_TRUE(used);
    }
}
