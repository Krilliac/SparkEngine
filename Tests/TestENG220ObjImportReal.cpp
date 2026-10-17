/**
 * @file TestENG220ObjImportReal.cpp
 * @brief ENG-220: production MeshAsset::Load OBJ import contract on every platform.
 *
 * The D3D11 draw list and shadow pass render ECS meshes through
 * AssetPipeline -> MeshAsset::Load. These tests drive that production entry
 * point (with a WARP device on Windows, CPU-only elsewhere) against canonical
 * Blender-exported OBJ files that ship in Assets/, and compare the imported
 * corners with an independent line-by-line reading of the same OBJ text.
 *
 * Contract:
 *  - authored `v//vn` normals survive import (they must not become zero);
 *  - OBJ bottom-left UVs are converted to the engine's top-left convention,
 *    matching Mesh::LoadFromFile and the portable importer;
 *  - an existing OBJ that yields no triangles fails instead of silently
 *    turning into a placeholder cube;
 *  - OBJ files without normals receive unit-length generated normals, and
 *    non-unit authored normals (Kenney repair_tool.obj writes 0.5) are
 *    normalized;
 *  - every authored n-gon yields n - 2 triangles (tinyobjloader's built-in
 *    ear clipper dropped 27 of carbine_a.obj's 1164 triangles).
 *
 * The only fixtures the tests write are small text OBJ files in a temporary
 * directory; no binary data is invented.
 */

#include "TestFramework.h"
#include "Core/Platform.h"
#include "Graphics/AssetPipeline.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

namespace
{
    /// position xyz, normal xyz, texcoord uv
    using Corner = std::array<float, 8>;

    struct ExpectedObj
    {
        std::vector<Corner> corners; ///< distinct authored corners
        size_t triangleCount = 0;    ///< sum over faces of (vertexCount - 2)
    };

    /// Independent reading of the OBJ text: positive 1-based indices only, which
    /// is what the canonical Blender exports contain. UV v is flipped to the
    /// engine's top-left origin.
    ExpectedObj ReadObjIndependently(const std::filesystem::path& path)
    {
        ExpectedObj expected;
        std::ifstream file(path);
        std::vector<std::array<float, 3>> positions;
        std::vector<std::array<float, 3>> normals;
        std::vector<std::array<float, 2>> texCoords;
        std::string line;
        while (std::getline(file, line))
        {
            std::istringstream stream(line);
            std::string prefix;
            stream >> prefix;
            if (prefix == "v")
            {
                std::array<float, 3> p{};
                stream >> p[0] >> p[1] >> p[2];
                positions.push_back(p);
            }
            else if (prefix == "vn")
            {
                std::array<float, 3> n{};
                stream >> n[0] >> n[1] >> n[2];
                normals.push_back(n);
            }
            else if (prefix == "vt")
            {
                std::array<float, 2> t{};
                stream >> t[0] >> t[1];
                texCoords.push_back(t);
            }
            else if (prefix == "f")
            {
                size_t faceVertices = 0;
                std::string token;
                while (stream >> token)
                {
                    std::array<std::string, 3> parts;
                    size_t part = 0;
                    for (char c : token)
                    {
                        if (c == '/')
                            ++part;
                        else if (part < 3)
                            parts[part] += c;
                    }
                    Corner corner{};
                    const int vi = std::atoi(parts[0].c_str());
                    const int ti = parts[1].empty() ? 0 : std::atoi(parts[1].c_str());
                    const int ni = parts[2].empty() ? 0 : std::atoi(parts[2].c_str());
                    if (vi <= 0 || static_cast<size_t>(vi) > positions.size())
                        continue;
                    corner[0] = positions[vi - 1][0];
                    corner[1] = positions[vi - 1][1];
                    corner[2] = positions[vi - 1][2];
                    if (ni > 0 && static_cast<size_t>(ni) <= normals.size())
                    {
                        corner[3] = normals[ni - 1][0];
                        corner[4] = normals[ni - 1][1];
                        corner[5] = normals[ni - 1][2];
                    }
                    if (ti > 0 && static_cast<size_t>(ti) <= texCoords.size())
                    {
                        corner[6] = texCoords[ti - 1][0];
                        corner[7] = 1.0f - texCoords[ti - 1][1];
                    }
                    expected.corners.push_back(corner);
                    ++faceVertices;
                }
                if (faceVertices >= 3)
                    expected.triangleCount += faceVertices - 2;
            }
        }
        return expected;
    }

    bool NearlyEqual(const Corner& a, const Corner& b, size_t firstComponent, size_t lastComponent)
    {
        for (size_t i = firstComponent; i <= lastComponent; ++i)
        {
            if (std::fabs(a[i] - b[i]) > 1.0e-4f)
                return false;
        }
        return true;
    }

    bool ContainsCorner(const std::vector<Corner>& corners, const Corner& needle, size_t firstComponent,
                        size_t lastComponent)
    {
        return std::any_of(corners.begin(), corners.end(), [&](const Corner& candidate)
                           { return NearlyEqual(candidate, needle, firstComponent, lastComponent); });
    }

    std::vector<Corner> ImportedCorners(const MeshAssetData& mesh)
    {
        std::vector<Corner> corners;
        corners.reserve(mesh.indices.size());
        for (uint32_t index : mesh.indices)
        {
            const auto& v = mesh.vertices[index];
            corners.push_back({v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z,
                               v.texCoord0.x, v.texCoord0.y});
        }
        return corners;
    }

    /// Runs production MeshAsset::Load. On Windows the D3D11 implementation needs
    /// a device; WARP keeps the test independent of GPU hardware.
    HRESULT LoadThroughProductionPath(MeshAsset& asset)
    {
#ifdef SPARK_PLATFORM_WINDOWS
        Microsoft::WRL::ComPtr<ID3D11Device> device;
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL featureLevel{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     device.GetAddressOf(), &featureLevel, context.GetAddressOf())))
        {
            SKIP_TEST("WARP D3D11 device unavailable");
        }
        return asset.Load(device.Get());
#else
        return asset.Load(nullptr);
#endif
    }

    std::filesystem::path CanonicalModel(const char* relative)
    {
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR) / relative;
    }

    std::filesystem::path WriteTextFixture(const char* name, const char* contents)
    {
        std::error_code ec;
        const auto dir = std::filesystem::temp_directory_path(ec) / "SparkENG220ObjImport";
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / name;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << contents;
        return path;
    }

    void CheckCanonicalCorners(const char* relative, size_t firstComponent, size_t lastComponent)
    {
        const auto path = CanonicalModel(relative);
        ASSERT_TRUE(std::filesystem::is_regular_file(path));
        const ExpectedObj expected = ReadObjIndependently(path);
        ASSERT_TRUE(expected.triangleCount > 0);

        MeshAsset asset(path.string());
        ASSERT_TRUE(SUCCEEDED(LoadThroughProductionPath(asset)));
        const MeshAssetData& mesh = asset.GetMeshData();
        ASSERT_EQ(mesh.indices.size() % 3, size_t{0});
        EXPECT_EQ(mesh.indices.size() / 3, expected.triangleCount);
        for (uint32_t index : mesh.indices)
            ASSERT_TRUE(index < mesh.vertices.size());

        const std::vector<Corner> imported = ImportedCorners(mesh);
        size_t missingFromImport = 0;
        for (const Corner& corner : expected.corners)
        {
            if (!ContainsCorner(imported, corner, firstComponent, lastComponent))
                ++missingFromImport;
        }
        size_t notAuthored = 0;
        for (const Corner& corner : imported)
        {
            if (!ContainsCorner(expected.corners, corner, firstComponent, lastComponent))
                ++notAuthored;
        }
        EXPECT_EQ(missingFromImport, size_t{0});
        EXPECT_EQ(notAuthored, size_t{0});
    }
} // namespace

// Assets/Models/MMOFPS/weapons/carbine_a.obj is a Blender 2.79 export whose
// faces are written as `v//vn` (no texture coordinates).
TEST(ENG220_ObjImport_SlashSlashNormalsSurviveProductionLoad)
{
    CheckCanonicalCorners("Assets/Models/MMOFPS/weapons/carbine_a.obj", 0, 5);

    MeshAsset asset(CanonicalModel("Assets/Models/MMOFPS/weapons/carbine_a.obj").string());
    ASSERT_TRUE(SUCCEEDED(LoadThroughProductionPath(asset)));
    size_t nonUnitNormals = 0;
    for (const auto& vertex : asset.GetMeshData().vertices)
    {
        const double lengthSquared = double(vertex.normal.x) * vertex.normal.x +
                                     double(vertex.normal.y) * vertex.normal.y +
                                     double(vertex.normal.z) * vertex.normal.z;
        if (std::fabs(lengthSquared - 1.0) > 0.002)
            ++nonUnitNormals;
    }
    EXPECT_EQ(nonUnitNormals, size_t{0});
}

// Assets/Models/Cube.obj is a Blender 5.2 export with `v/vt/vn` faces.
TEST(ENG220_ObjImport_TexCoordsUseEngineTopLeftOrigin)
{
    CheckCanonicalCorners("Assets/Models/Cube.obj", 0, 7);
}

// Every OBJ under Assets/ must import completely: one triangle per authored
// polygon corner beyond the second, in-range indices, and unit normals.
TEST(ENG220_ObjImport_EveryCanonicalAssetObjImportsCompletely)
{
    const auto root = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Assets";
    ASSERT_TRUE(std::filesystem::is_directory(root));

    size_t filesChecked = 0;
    std::vector<std::string> incomplete;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root))
    {
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!entry.is_regular_file() || extension != ".obj")
            continue;
        ++filesChecked;

        const ExpectedObj expected = ReadObjIndependently(entry.path());
        MeshAsset asset(entry.path().string());
        if (FAILED(LoadThroughProductionPath(asset)))
        {
            incomplete.push_back(entry.path().string() + ": load failed");
            continue;
        }
        const MeshAssetData& mesh = asset.GetMeshData();
        if (mesh.indices.size() != expected.triangleCount * 3)
        {
            incomplete.push_back(entry.path().string() + ": " + std::to_string(mesh.indices.size() / 3) + " of " +
                                 std::to_string(expected.triangleCount) + " triangles");
            continue;
        }
        size_t badNormals = 0;
        for (uint32_t index : mesh.indices)
        {
            if (index >= mesh.vertices.size())
            {
                ++badNormals;
                continue;
            }
            const auto& n = mesh.vertices[index].normal;
            const double lengthSquared = double(n.x) * n.x + double(n.y) * n.y + double(n.z) * n.z;
            if (std::fabs(lengthSquared - 1.0) > 0.002)
                ++badNormals;
        }
        if (badNormals != 0)
            incomplete.push_back(entry.path().string() + ": " + std::to_string(badNormals) + " bad corners");
    }

    for (const std::string& message : incomplete)
        std::printf("  incomplete OBJ import: %s\n", message.c_str());
    // Guard against a vacuous pass if the asset tree moves.
    EXPECT_TRUE(filesChecked >= 100);
    EXPECT_EQ(incomplete.size(), size_t{0});
}

TEST(ENG220_ObjImport_ObjWithoutTrianglesFailsInsteadOfPlaceholderCube)
{
    const auto path = WriteTextFixture("no_faces.obj", "# vertices but no faces\n"
                                                       "v 0 0 0\n"
                                                       "v 1 0 0\n"
                                                       "v 0 1 0\n");
    MeshAsset asset(path.string());
    const HRESULT hr = LoadThroughProductionPath(asset);
    EXPECT_TRUE(FAILED(hr));
    EXPECT_FALSE(asset.IsLoaded());
    EXPECT_TRUE(asset.GetMeshData().indices.empty());
}

TEST(ENG220_ObjImport_MissingNormalsAreGeneratedUnitLength)
{
    const auto path = WriteTextFixture("no_normals.obj", "v 0 0 0\n"
                                                         "v 1 0 0\n"
                                                         "v 0 1 0\n"
                                                         "f 1 2 3\n");
    MeshAsset asset(path.string());
    ASSERT_TRUE(SUCCEEDED(LoadThroughProductionPath(asset)));
    const MeshAssetData& mesh = asset.GetMeshData();
    ASSERT_EQ(mesh.indices.size(), size_t{3});
    for (uint32_t index : mesh.indices)
    {
        const auto& n = mesh.vertices[index].normal;
        // Counter-clockwise triangle in the XY plane: geometric normal is +Z.
        EXPECT_NEAR(n.x, 0.0f, 1.0e-5f);
        EXPECT_NEAR(n.y, 0.0f, 1.0e-5f);
        EXPECT_NEAR(n.z, 1.0f, 1.0e-5f);
    }
}
