/**
 * @file TestGLTFStaticMeshLoader.cpp
 * @brief CPU-only tests for the fail-closed glTF 2.0 static-mesh subset.
 */

#include "TestFramework.h"
#include "Graphics/GLTFStaticMeshLoader.h"

#include <bit>
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
