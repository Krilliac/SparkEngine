/**
 * @file RHI210D3D11GoldenSupport.h
 * @brief Shared helpers for the RHI-210 d3d11-warp golden lanes.
 *
 * Used by the TestRHI210D3D11*GoldenReal.cpp lanes (one fixture per lane file,
 * see Tests/GoldenImages/README.md). It holds three things:
 *
 *   - MatchesGolden(): the comparison through GoldenImageTestRunner, which reads
 *     the reviewed thresholds and baseline SHA-256 from
 *     Tests/GoldenImages/manifest.json and fails closed. The actual frame is kept
 *     in SPARK_GOLDEN_OUTPUT_DIR (default <cwd>/Tests/Output) as
 *     <row>_<scene>.png on every failure, and on every run when
 *     SPARK_GOLDEN_WRITE_ACTUAL=1 (baseline capture, run-to-run variance), next to
 *     the CPU reference image <scene>_cpu.png.
 *   - ShadeBasic(): a CPU evaluation of the engine's embedded basic pixel shader
 *     (GraphicsDeviceResourcesWindowsShaders.cpp) under the default per-frame light
 *     of GraphicsEngine::UpdateFrameConstants.
 *   - A CPU ray caster over the same triangles the GPU draws (the OBJ files a
 *     GameObject loads, an AssetPipeline mesh's data, Mesh::CreateCube /
 *     CreatePlane), with back-face culling, perspective-correct barycentric
 *     attributes and correctly transformed normals. Lanes shade its hits with
 *     ShadeBasic and compare every interior pixel of a frame, so a broken render
 *     cannot become a baseline.
 *
 * Test-only code. Thread affinity: the calling test thread. Allocation: per call.
 */
#pragma once

#ifdef _WIN32

#include "Graphics/AssetPipeline.h"
#include "Graphics/Mesh.h"
#include "Utils/GoldenImageTest.h"

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace RHI210Golden
{
    using Color3 = std::array<float, 3>;

    inline std::filesystem::path GoldenDir()
    {
        return std::filesystem::path(SPARK_TEST_SOURCE_DIR) / "Tests" / "GoldenImages";
    }

    inline std::filesystem::path OutputDir()
    {
        const char* overrideDir = std::getenv("SPARK_GOLDEN_OUTPUT_DIR");
        if (overrideDir != nullptr && overrideDir[0] != '\0')
        {
            return overrideDir;
        }
        return std::filesystem::current_path() / "Tests" / "Output";
    }

    inline bool WriteActualRequested()
    {
        const char* writeActual = std::getenv("SPARK_GOLDEN_WRITE_ACTUAL");
        return writeActual != nullptr && std::string(writeActual) == "1";
    }

    /// Test-only negative control. This never changes the expected pixels or thresholds.
    inline bool PassDisabled(const char* pass)
    {
        const char* disabled = std::getenv("SPARK_RHI210_DISABLE_PASS");
        return disabled != nullptr && std::string(disabled) == pass;
    }

    /// Hands a frame that was already read back to GoldenImageTestRunner.
    class ReadbackCapture final : public Spark::IGoldenImageCapture
    {
      public:
        ReadbackCapture(std::vector<uint8_t> pixels, uint32_t width, uint32_t height)
            : m_pixels(std::move(pixels)), m_width(width), m_height(height)
        {
        }

        std::vector<uint8_t> CaptureFramebuffer(uint32_t width, uint32_t height) override
        {
            // A baseline of another size fails the runner's size check.
            if (width != m_width || height != m_height)
            {
                return {};
            }
            return m_pixels;
        }

      private:
        std::vector<uint8_t> m_pixels;
        uint32_t m_width;
        uint32_t m_height;
    };

    /// Compares an RGBA8 frame with the reviewed baseline of @p scene on @p row.
    inline bool MatchesGolden(const char* row, const char* scene, const std::vector<uint8_t>& rgba, uint32_t width,
                              uint32_t height)
    {
        auto& runner = Spark::GoldenImageTestRunner::GetInstance();
        Spark::GoldenImageConfig config;
        config.goldenImageDir = GoldenDir().string();
        config.outputDir = OutputDir().string();
        config.backendRow = row;
        runner.Initialize(config);
        runner.SetCapture(std::make_unique<ReadbackCapture>(rgba, width, height));
        const Spark::ImageComparisonResult result = runner.CompareWithGolden(scene);
        runner.Shutdown();

        std::printf("[RHI-210 GOLDEN] scene=%s matched=%s differing=%u/%u (%.3f%%, tolerance %.3f%%) "
                    "maxDist=%.2f meanDist=%.3f threshold=%.2f%s%s\n",
                    scene, result.matched ? "yes" : "no", result.differentPixels, result.totalPixels,
                    result.percentDifferent, result.tolerancePercent, result.maxPixelDistance,
                    result.averagePixelDistance, result.perPixelThreshold,
                    result.failureReason.empty() ? "" : " reason=", result.failureReason.c_str());

        // The runner keeps the actual frame only on a pixel failure; keep it on every
        // failure (missing entry, hash mismatch) so a new baseline can be reviewed,
        // and on request so repeated runs can be compared byte for byte.
        if (!result.matched || WriteActualRequested())
        {
            const std::filesystem::path actual = OutputDir() / (std::string(row) + "_" + scene + ".png");
            std::error_code ec;
            std::filesystem::create_directories(actual.parent_path(), ec);
            if (Spark::GoldenImageTestRunner::SavePNG(actual.string(), rgba.data(), width, height))
            {
                std::printf("[RHI-210 GOLDEN] actual frame written to %s\n", actual.string().c_str());
            }
        }
        return result.matched;
    }

    // ------------------------------------------------------------------------
    // Pixel access
    // ------------------------------------------------------------------------

    inline std::array<int, 3> PixelAt(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y)
    {
        const size_t i = (size_t(y) * width + x) * 4;
        return {rgba[i + 0], rgba[i + 1], rgba[i + 2]};
    }

    inline int ToUnorm8(float value)
    {
        return static_cast<int>(std::lround(std::clamp(value, 0.0f, 1.0f) * 255.0f));
    }

    /// Largest per-channel difference between the pixel and the colour (saturated to UNORM8).
    inline int ChannelError(const std::array<int, 3>& pixel, const Color3& expected)
    {
        int worst = 0;
        for (int c = 0; c < 3; ++c)
        {
            worst = std::max(worst, std::abs(pixel[c] - ToUnorm8(expected[c])));
        }
        return worst;
    }

    // ------------------------------------------------------------------------
    // CPU reference of the embedded basic pixel shader
    // ------------------------------------------------------------------------

    /// GraphicsEngine::UpdateFrameConstants' light when SetEnvironmentLighting was never called.
    struct BasicFrameLight
    {
        DirectX::XMFLOAT3 direction{0.35f, -0.8f, 0.45f};
        float intensity = 1.0f;
        DirectX::XMFLOAT3 color{1.0f, 0.97f, 0.9f};
        float ambientIntensity = 0.6f;
        DirectX::XMFLOAT3 ambientColor{0.52f, 0.5f, 0.46f};
    };

    /**
     * @brief The basic pixel shader for one surface point.
     *
     * finalColor = albedo * (L * I * N.L + ambient + view fill) + specular, where the
     * view fill is 0.35 * max(0, N.V) tinted (1, 0.95, 0.88) and the Blinn-Phong
     * specular is scaled by the gloss 1 - roughness (zero for the default fully rough
     * binding). @p normal is the shading normal after normal mapping, unit length;
     * @p albedo already includes the texture sample and ObjectColor.
     */
    inline Color3 ShadeBasic(const Color3& albedo, const DirectX::XMFLOAT3& normal, const DirectX::XMFLOAT3& position,
                             const DirectX::XMFLOAT3& cameraPosition, float roughness = 1.0f,
                             const BasicFrameLight& light = {})
    {
        using namespace DirectX;
        const XMVECTOR n = XMLoadFloat3(&normal);
        const XMVECTOR l = XMVector3Normalize(XMVectorNegate(XMLoadFloat3(&light.direction)));
        const XMVECTOR v = XMVector3Normalize(XMVectorSubtract(XMLoadFloat3(&cameraPosition), XMLoadFloat3(&position)));
        const float nDotL = std::max(0.0f, XMVectorGetX(XMVector3Dot(n, l)));
        const float fill = 0.35f * std::max(0.0f, XMVectorGetX(XMVector3Dot(n, v)));
        const std::array<float, 3> fillTint = {1.0f, 0.95f, 0.88f};

        const float gloss = 1.0f - std::clamp(roughness, 0.0f, 1.0f);
        const float nDotH = std::max(0.0f, XMVectorGetX(XMVector3Dot(n, XMVector3Normalize(XMVectorAdd(l, v)))));
        const float specPower = 8.0f + (96.0f - 8.0f) * gloss;
        const float specular =
            light.intensity * std::pow(nDotH, specPower) * gloss * 0.5f * std::clamp(nDotL * 4.0f, 0.0f, 1.0f);

        const std::array<float, 3> lightColor = {light.color.x, light.color.y, light.color.z};
        const std::array<float, 3> ambientColor = {light.ambientColor.x, light.ambientColor.y, light.ambientColor.z};
        Color3 out{};
        for (int c = 0; c < 3; ++c)
        {
            const float lighting =
                lightColor[c] * light.intensity * nDotL + ambientColor[c] * light.ambientIntensity + fill * fillTint[c];
            out[c] = albedo[c] * lighting + lightColor[c] * specular;
        }
        return out;
    }

    // ------------------------------------------------------------------------
    // Triangle meshes, as the GPU receives them
    // ------------------------------------------------------------------------

    /// Triangle soup with per-corner attributes in mesh-local space (3 corners per triangle).
    struct TriangleMesh
    {
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT3> normals;
        std::vector<DirectX::XMFLOAT2> uvs;
        DirectX::XMFLOAT3 boundsMin{0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 boundsMax{0.0f, 0.0f, 0.0f};

        size_t TriangleCount() const { return positions.size() / 3; }

        void ComputeBounds()
        {
            boundsMin = {1.0e30f, 1.0e30f, 1.0e30f};
            boundsMax = {-1.0e30f, -1.0e30f, -1.0e30f};
            for (const auto& p : positions)
            {
                boundsMin = {std::min(boundsMin.x, p.x), std::min(boundsMin.y, p.y), std::min(boundsMin.z, p.z)};
                boundsMax = {std::max(boundsMax.x, p.x), std::max(boundsMax.y, p.y), std::max(boundsMax.z, p.z)};
            }
        }
    };

    /// Read the buffers the production Mesh actually uploaded; the reference does not guess its primitive shape.
    template <typename T>
    inline bool ReadBuffer(ID3D11Buffer* source, ID3D11Device* device, ID3D11DeviceContext* context,
                           std::vector<T>& elements)
    {
        if (!source || !device || !context)
        {
            return false;
        }
        D3D11_BUFFER_DESC desc{};
        source->GetDesc(&desc);
        if (desc.ByteWidth == 0 || desc.ByteWidth % sizeof(T) != 0)
        {
            return false;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.MiscFlags = 0;
        desc.StructureByteStride = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Microsoft::WRL::ComPtr<ID3D11Buffer> staging;
        if (FAILED(device->CreateBuffer(&desc, nullptr, &staging)))
        {
            return false;
        }
        context->CopyResource(staging.Get(), source);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
        {
            return false;
        }
        elements.resize(desc.ByteWidth / sizeof(T));
        std::memcpy(elements.data(), mapped.pData, desc.ByteWidth);
        context->Unmap(staging.Get(), 0);
        return true;
    }

    inline std::optional<TriangleMesh> ReadMeshBuffers(const Mesh& source, ID3D11Device* device,
                                                       ID3D11DeviceContext* context)
    {
        std::vector<Vertex> vertices;
        std::vector<unsigned int> indices;
        if (!ReadBuffer(source.GetVertexBuffer(), device, context, vertices) ||
            !ReadBuffer(source.GetIndexBuffer(), device, context, indices) || indices.size() % 3 != 0)
        {
            return std::nullopt;
        }
        TriangleMesh mesh;
        for (const unsigned int index : indices)
        {
            if (index >= vertices.size())
            {
                return std::nullopt;
            }
            mesh.positions.push_back(vertices[index].Position);
            mesh.normals.push_back(vertices[index].Normal);
            mesh.uvs.push_back(vertices[index].TexCoord);
        }
        mesh.ComputeBounds();
        return mesh;
    }

    /// Mesh::CalculateNormals: each triangle writes its face normal to its corners.
    inline void ApplyFaceNormals(TriangleMesh& mesh)
    {
        using namespace DirectX;
        for (size_t t = 0; t < mesh.TriangleCount(); ++t)
        {
            const XMVECTOR p0 = XMLoadFloat3(&mesh.positions[t * 3 + 0]);
            const XMVECTOR p1 = XMLoadFloat3(&mesh.positions[t * 3 + 1]);
            const XMVECTOR p2 = XMLoadFloat3(&mesh.positions[t * 3 + 2]);
            XMFLOAT3 n;
            XMStoreFloat3(&n, XMVector3Normalize(XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0))));
            mesh.normals[t * 3 + 0] = n;
            mesh.normals[t * 3 + 1] = n;
            mesh.normals[t * 3 + 2] = n;
        }
    }

    /**
     * @brief An OBJ as Mesh::LoadFromFile uploads it for a GameObject: positions and
     *        normals as written, V flipped to the top-left origin, {0, 1, 0} for a
     *        corner without a normal (all corners re-derived from the faces when any
     *        normal is zero). The shipped models are already triangulated.
     */
    inline std::optional<TriangleMesh> LoadObjAsGameObjectMesh(const std::filesystem::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        if (!file)
        {
            return std::nullopt;
        }
        std::vector<DirectX::XMFLOAT3> positions;
        std::vector<DirectX::XMFLOAT3> normals;
        std::vector<DirectX::XMFLOAT2> uvs;
        TriangleMesh mesh;
        bool anyZeroNormal = false;
        std::string line;
        auto resolve = [](int index, size_t count) { return index < 0 ? int(count) + index : index - 1; };
        while (std::getline(file, line))
        {
            std::istringstream in(line);
            std::string tag;
            in >> tag;
            if (tag == "v")
            {
                DirectX::XMFLOAT3 p{};
                in >> p.x >> p.y >> p.z;
                positions.push_back(p);
            }
            else if (tag == "vn")
            {
                DirectX::XMFLOAT3 n{};
                in >> n.x >> n.y >> n.z;
                normals.push_back(n);
            }
            else if (tag == "vt")
            {
                DirectX::XMFLOAT2 t{};
                in >> t.x >> t.y;
                uvs.push_back(t);
            }
            else if (tag == "f")
            {
                std::vector<std::array<int, 3>> corners;
                std::string token;
                while (in >> token)
                {
                    std::array<int, 3> corner = {0, 0, 0};
                    std::istringstream parts(token);
                    std::string part;
                    for (int field = 0; field < 3 && std::getline(parts, part, '/'); ++field)
                    {
                        corner[size_t(field)] = part.empty() ? 0 : std::stoi(part);
                    }
                    corners.push_back(corner);
                }
                // Triangle fan, as tinyobj's "simple" triangulation.
                for (size_t k = 1; k + 1 < corners.size(); ++k)
                {
                    for (const size_t c : {size_t(0), k, k + 1})
                    {
                        const auto& corner = corners[c];
                        mesh.positions.push_back(positions[size_t(resolve(corner[0], positions.size()))]);
                        DirectX::XMFLOAT2 uv{0.0f, 0.0f};
                        if (corner[1] != 0)
                        {
                            const DirectX::XMFLOAT2 t = uvs[size_t(resolve(corner[1], uvs.size()))];
                            uv = {t.x, 1.0f - t.y};
                        }
                        mesh.uvs.push_back(uv);
                        DirectX::XMFLOAT3 n{0.0f, 1.0f, 0.0f};
                        if (corner[2] != 0)
                        {
                            n = normals[size_t(resolve(corner[2], normals.size()))];
                        }
                        anyZeroNormal = anyZeroNormal || (n.x == 0.0f && n.y == 0.0f && n.z == 0.0f);
                        mesh.normals.push_back(n);
                    }
                }
            }
        }
        if (mesh.positions.empty())
        {
            return std::nullopt;
        }
        if (anyZeroNormal)
        {
            ApplyFaceNormals(mesh);
        }
        mesh.ComputeBounds();
        return mesh;
    }

    /// An AssetPipeline mesh as the draw list uploads it (position, normal, texCoord0).
    inline TriangleMesh FromMeshAssetData(const MeshAssetData& data)
    {
        TriangleMesh mesh;
        for (const uint32_t index : data.indices)
        {
            const MeshAssetData::Vertex& vertex = data.vertices[index];
            mesh.positions.push_back(vertex.position);
            mesh.normals.push_back(vertex.normal);
            mesh.uvs.push_back(vertex.texCoord0);
        }
        mesh.ComputeBounds();
        return mesh;
    }

    /// Mesh::CreateCube(size): 24 vertices with authored per-face normals and UVs.
    inline TriangleMesh ProceduralCube(float size)
    {
        const float h = size * 0.5f;
        struct V
        {
            DirectX::XMFLOAT3 p;
            DirectX::XMFLOAT3 n;
            DirectX::XMFLOAT2 t;
        };
        const V v[24] = {
            {{-h, -h, h}, {0, 0, 1}, {0, 1}},  {{h, -h, h}, {0, 0, 1}, {1, 1}},    {{h, h, h}, {0, 0, 1}, {1, 0}},
            {{-h, h, h}, {0, 0, 1}, {0, 0}},   {{h, -h, -h}, {0, 0, -1}, {0, 1}},  {{-h, -h, -h}, {0, 0, -1}, {1, 1}},
            {{-h, h, -h}, {0, 0, -1}, {1, 0}}, {{h, h, -h}, {0, 0, -1}, {0, 0}},   {{-h, -h, -h}, {-1, 0, 0}, {0, 1}},
            {{-h, -h, h}, {-1, 0, 0}, {1, 1}}, {{-h, h, h}, {-1, 0, 0}, {1, 0}},   {{-h, h, -h}, {-1, 0, 0}, {0, 0}},
            {{h, -h, h}, {1, 0, 0}, {0, 1}},   {{h, -h, -h}, {1, 0, 0}, {1, 1}},   {{h, h, -h}, {1, 0, 0}, {1, 0}},
            {{h, h, h}, {1, 0, 0}, {0, 0}},    {{-h, -h, -h}, {0, -1, 0}, {0, 1}}, {{h, -h, -h}, {0, -1, 0}, {1, 1}},
            {{h, -h, h}, {0, -1, 0}, {1, 0}},  {{-h, -h, h}, {0, -1, 0}, {0, 0}},  {{-h, h, h}, {0, 1, 0}, {0, 1}},
            {{h, h, h}, {0, 1, 0}, {1, 1}},    {{h, h, -h}, {0, 1, 0}, {1, 0}},    {{-h, h, -h}, {0, 1, 0}, {0, 0}},
        };
        TriangleMesh mesh;
        for (int face = 0; face < 6; ++face)
        {
            const int b = face * 4;
            for (const int i : {b, b + 1, b + 2, b, b + 2, b + 3})
            {
                mesh.positions.push_back(v[i].p);
                mesh.normals.push_back(v[i].n);
                mesh.uvs.push_back(v[i].t);
            }
        }
        mesh.ComputeBounds();
        return mesh;
    }

    /// Mesh::CreatePlane(width, depth): two triangles, normals re-derived by
    /// CreateFromVertices' CalculateNormals from the winding.
    inline TriangleMesh ProceduralPlane(float width, float depth)
    {
        const float hw = width * 0.5f;
        const float hd = depth * 0.5f;
        const DirectX::XMFLOAT3 p[4] = {{-hw, 0, -hd}, {hw, 0, -hd}, {hw, 0, hd}, {-hw, 0, hd}};
        const DirectX::XMFLOAT2 t[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
        TriangleMesh mesh;
        for (const int i : {0, 1, 2, 0, 2, 3})
        {
            mesh.positions.push_back(p[i]);
            mesh.normals.push_back({0.0f, 1.0f, 0.0f});
            mesh.uvs.push_back(t[i]);
        }
        ApplyFaceNormals(mesh);
        mesh.ComputeBounds();
        return mesh;
    }

    /// Mesh::CreateSphere(radius, slices, stacks): an indexed grid whose shared vertices
    /// end up with the face normal of the last triangle CalculateNormals visits.
    inline TriangleMesh ProceduralSphere(float radius, int slices, int stacks)
    {
        using namespace DirectX;
        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT2> uvs;
        for (int i = 0; i <= stacks; ++i)
        {
            const float v = i / static_cast<float>(stacks);
            const float phi = v * XM_PI;
            for (int j = 0; j <= slices; ++j)
            {
                const float u = j / static_cast<float>(slices);
                const float theta = u * XM_2PI;
                positions.push_back(
                    {radius * sinf(phi) * cosf(theta), radius * cosf(phi), radius * sinf(phi) * sinf(theta)});
                uvs.push_back({u, v});
            }
        }
        std::vector<uint32_t> indices;
        for (int i = 0; i < stacks; ++i)
        {
            for (int j = 0; j < slices; ++j)
            {
                const auto a = static_cast<uint32_t>(i * (slices + 1) + j);
                const auto b = a + static_cast<uint32_t>(slices) + 1;
                indices.insert(indices.end(), {a, b, a + 1, b, b + 1, a + 1});
            }
        }
        std::vector<XMFLOAT3> normals(positions.size(), XMFLOAT3{0.0f, 1.0f, 0.0f});
        for (size_t i = 0; i + 2 < indices.size(); i += 3)
        {
            const XMVECTOR p0 = XMLoadFloat3(&positions[indices[i]]);
            const XMVECTOR p1 = XMLoadFloat3(&positions[indices[i + 1]]);
            const XMVECTOR p2 = XMLoadFloat3(&positions[indices[i + 2]]);
            XMFLOAT3 n;
            XMStoreFloat3(&n, XMVector3Normalize(XMVector3Cross(XMVectorSubtract(p1, p0), XMVectorSubtract(p2, p0))));
            normals[indices[i]] = n;
            normals[indices[i + 1]] = n;
            normals[indices[i + 2]] = n;
        }
        TriangleMesh mesh;
        for (const uint32_t index : indices)
        {
            mesh.positions.push_back(positions[index]);
            mesh.normals.push_back(normals[index]);
            mesh.uvs.push_back(uvs[index]);
        }
        mesh.ComputeBounds();
        return mesh;
    }

    // ------------------------------------------------------------------------
    // Ray casting
    // ------------------------------------------------------------------------

    struct Ray
    {
        DirectX::XMFLOAT3 origin{};
        DirectX::XMFLOAT3 direction{};
    };

    /// The ray through the point (x, y) of a width x height viewport, in pixels (a
    /// pixel centre is integer + 0.5).
    inline Ray ViewportRay(float x, float y, uint32_t width, uint32_t height, const DirectX::XMMATRIX& view,
                           const DirectX::XMMATRIX& projection)
    {
        using namespace DirectX;
        const XMMATRIX inverse = XMMatrixInverse(nullptr, XMMatrixMultiply(view, projection));
        const float ndcX = x / float(width) * 2.0f - 1.0f;
        const float ndcY = 1.0f - y / float(height) * 2.0f;
        const XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverse);
        const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverse);
        Ray ray;
        XMStoreFloat3(&ray.origin, nearPoint);
        XMStoreFloat3(&ray.direction, XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint)));
        return ray;
    }

    /// A mesh placed in the world, as one draw.
    struct Instance
    {
        const TriangleMesh* mesh = nullptr;
        bool twoSided = false;
        DirectX::XMFLOAT4X4 world{};
        DirectX::XMFLOAT4X4 toLocal{};
        DirectX::XMFLOAT4X4 normalMatrix{}; ///< Inverse transpose of world: n_world = n_local * normalMatrix.
    };

    inline Instance MakeInstance(const TriangleMesh& mesh, const DirectX::XMMATRIX& world)
    {
        using namespace DirectX;
        Instance instance;
        instance.mesh = &mesh;
        XMStoreFloat4x4(&instance.world, world);
        const XMMATRIX toLocal = XMMatrixInverse(nullptr, world);
        XMStoreFloat4x4(&instance.toLocal, toLocal);
        XMStoreFloat4x4(&instance.normalMatrix, XMMatrixTranspose(toLocal));
        return instance;
    }

    struct SurfaceHit
    {
        int instance = -1;
        int triangle = -1;
        float distance = 0.0f;
        DirectX::XMFLOAT3 position{};   ///< World position.
        DirectX::XMFLOAT3 normal{};     ///< World shading normal, as the basic VS/PS interpolate it.
        DirectX::XMFLOAT3 faceNormal{}; ///< World geometric normal of the triangle.
        DirectX::XMFLOAT2 uv{};
    };

    /**
     * @brief Nearest front-facing triangle hit (D3D11 CULL_BACK, clockwise front faces:
     *        a triangle is front-facing when cross(p1 - p0, p2 - p0) points at the viewer).
     */
    inline std::optional<SurfaceHit> CastRay(const Ray& ray, const std::vector<Instance>& instances)
    {
        using namespace DirectX;
        std::optional<SurfaceHit> nearest;
        for (size_t i = 0; i < instances.size(); ++i)
        {
            const Instance& instance = instances[i];
            const TriangleMesh& mesh = *instance.mesh;
            const XMMATRIX toLocal = XMLoadFloat4x4(&instance.toLocal);
            const XMVECTOR origin = XMVector3TransformCoord(XMLoadFloat3(&ray.origin), toLocal);
            // Not normalized: the local parameter t then equals the world distance.
            const XMVECTOR direction = XMVector3TransformNormal(XMLoadFloat3(&ray.direction), toLocal);

            // Slab test against the local bounds first.
            float tMin = 0.0f;
            float tMax = nearest ? nearest->distance : 1.0e30f;
            const float o[3] = {XMVectorGetX(origin), XMVectorGetY(origin), XMVectorGetZ(origin)};
            const float d[3] = {XMVectorGetX(direction), XMVectorGetY(direction), XMVectorGetZ(direction)};
            const float lo[3] = {mesh.boundsMin.x - 1.0e-4f, mesh.boundsMin.y - 1.0e-4f, mesh.boundsMin.z - 1.0e-4f};
            const float hi[3] = {mesh.boundsMax.x + 1.0e-4f, mesh.boundsMax.y + 1.0e-4f, mesh.boundsMax.z + 1.0e-4f};
            bool overlaps = true;
            for (int axis = 0; axis < 3 && overlaps; ++axis)
            {
                if (std::abs(d[axis]) < 1.0e-12f)
                {
                    overlaps = o[axis] >= lo[axis] && o[axis] <= hi[axis];
                    continue;
                }
                const float t0 = (lo[axis] - o[axis]) / d[axis];
                const float t1 = (hi[axis] - o[axis]) / d[axis];
                tMin = std::max(tMin, std::min(t0, t1));
                tMax = std::min(tMax, std::max(t0, t1));
                overlaps = tMin <= tMax;
            }
            if (!overlaps)
            {
                continue;
            }

            for (size_t t = 0; t < mesh.TriangleCount(); ++t)
            {
                const XMVECTOR p0 = XMLoadFloat3(&mesh.positions[t * 3 + 0]);
                const XMVECTOR e1 = XMVectorSubtract(XMLoadFloat3(&mesh.positions[t * 3 + 1]), p0);
                const XMVECTOR e2 = XMVectorSubtract(XMLoadFloat3(&mesh.positions[t * 3 + 2]), p0);
                // Back faces are culled: front faces have cross(e1, e2) facing the viewer.
                if (!instance.twoSided && XMVectorGetX(XMVector3Dot(XMVector3Cross(e1, e2), direction)) >= 0.0f)
                {
                    continue;
                }
                // Moller-Trumbore.
                const XMVECTOR pvec = XMVector3Cross(direction, e2);
                const float det = XMVectorGetX(XMVector3Dot(e1, pvec));
                if (std::abs(det) < 1.0e-20f)
                {
                    continue;
                }
                const float invDet = 1.0f / det;
                const XMVECTOR tvec = XMVectorSubtract(origin, p0);
                const float u = XMVectorGetX(XMVector3Dot(tvec, pvec)) * invDet;
                if (u < 0.0f || u > 1.0f)
                {
                    continue;
                }
                const XMVECTOR qvec = XMVector3Cross(tvec, e1);
                const float v = XMVectorGetX(XMVector3Dot(direction, qvec)) * invDet;
                if (v < 0.0f || u + v > 1.0f)
                {
                    continue;
                }
                const float distance = XMVectorGetX(XMVector3Dot(e2, qvec)) * invDet;
                if (distance <= 1.0e-5f || (nearest && nearest->distance <= distance))
                {
                    continue;
                }

                SurfaceHit hit;
                hit.instance = static_cast<int>(i);
                hit.triangle = static_cast<int>(t);
                hit.distance = distance;
                const XMMATRIX world = XMLoadFloat4x4(&instance.world);
                const XMMATRIX normalMatrix = XMLoadFloat4x4(&instance.normalMatrix);
                XMStoreFloat3(&hit.position,
                              XMVector3TransformCoord(XMVectorAdd(origin, XMVectorScale(direction, distance)), world));
                const float w0 = 1.0f - u - v;
                // The VS normalizes each transformed vertex normal; the PS normalizes the
                // interpolated one.
                XMVECTOR normal = XMVectorZero();
                const float weights[3] = {w0, u, v};
                XMFLOAT2 uv{0.0f, 0.0f};
                for (int c = 0; c < 3; ++c)
                {
                    const XMVECTOR n = XMVector3Normalize(
                        XMVector3TransformNormal(XMLoadFloat3(&mesh.normals[t * 3 + size_t(c)]), normalMatrix));
                    normal = XMVectorAdd(normal, XMVectorScale(n, weights[c]));
                    uv.x += mesh.uvs[t * 3 + size_t(c)].x * weights[c];
                    uv.y += mesh.uvs[t * 3 + size_t(c)].y * weights[c];
                }
                XMStoreFloat3(&hit.normal, XMVector3Normalize(normal));
                XMStoreFloat3(&hit.faceNormal,
                              XMVector3Normalize(XMVector3TransformNormal(XMVector3Cross(e1, e2), normalMatrix)));
                hit.uv = uv;
                nearest = hit;
            }
        }
        return nearest;
    }

    /// One ray through every pixel centre of a frame.
    struct HitBuffer
    {
        uint32_t width = 0;
        uint32_t height = 0;
        std::vector<std::optional<SurfaceHit>> hits;

        const std::optional<SurfaceHit>& At(uint32_t x, uint32_t y) const { return hits[size_t(y) * width + x]; }
    };

    inline HitBuffer CastFrame(uint32_t width, uint32_t height, const DirectX::XMMATRIX& view,
                               const DirectX::XMMATRIX& projection, const std::vector<Instance>& instances)
    {
        HitBuffer buffer;
        buffer.width = width;
        buffer.height = height;
        buffer.hits.resize(size_t(width) * height);
        for (uint32_t y = 0; y < height; ++y)
        {
            for (uint32_t x = 0; x < width; ++x)
            {
                buffer.hits[size_t(y) * width + x] =
                    CastRay(ViewportRay(float(x) + 0.5f, float(y) + 0.5f, width, height, view, projection), instances);
            }
        }
        return buffer;
    }

    /// Same instance and the same face plane (two triangles of one flat face count as one surface).
    inline bool SameSurface(const SurfaceHit& a, const SurfaceHit& b)
    {
        using namespace DirectX;
        if (a.instance != b.instance)
        {
            return false;
        }
        const XMVECTOR na = XMLoadFloat3(&a.faceNormal);
        const XMVECTOR nb = XMLoadFloat3(&b.faceNormal);
        if (XMVectorGetX(XMVector3Dot(na, nb)) < 0.99999f)
        {
            return false;
        }
        // Coplanar: b's position lies in a's plane.
        const float offset =
            XMVectorGetX(XMVector3Dot(na, XMVectorSubtract(XMLoadFloat3(&b.position), XMLoadFloat3(&a.position))));
        return std::abs(offset) < 1.0e-3f;
    }

    enum class PixelClass
    {
        Edge,       ///< Silhouette, face edge or frame border: not probed.
        Background, ///< The pixel and its four neighbours miss every mesh.
        Interior,   ///< The pixel and its four neighbours lie on one flat surface.
    };

    inline PixelClass Classify(const HitBuffer& buffer, uint32_t x, uint32_t y)
    {
        if (x == 0 || y == 0 || x + 1 >= buffer.width || y + 1 >= buffer.height)
        {
            return PixelClass::Edge;
        }
        const std::optional<SurfaceHit>& centre = buffer.At(x, y);
        const std::optional<SurfaceHit>* neighbours[4] = {&buffer.At(x - 1, y), &buffer.At(x + 1, y),
                                                          &buffer.At(x, y - 1), &buffer.At(x, y + 1)};
        if (!centre)
        {
            for (const auto* neighbour : neighbours)
            {
                if (neighbour->has_value())
                {
                    return PixelClass::Edge;
                }
            }
            return PixelClass::Background;
        }
        for (const auto* neighbour : neighbours)
        {
            if (!neighbour->has_value() || !SameSurface(*centre, **neighbour))
            {
                return PixelClass::Edge;
            }
        }
        return PixelClass::Interior;
    }

    /**
     * @brief The basic pixel shader's CotangentFrame + normal-map step for a constant
     *        normal texel, with ddx/ddy taken from the hits at the next pixel centres.
     *
     * @p normalTexel is the 8-bit RGB of the normal map; @p tiling scales the UVs.
     */
    inline DirectX::XMFLOAT3 MappedNormal(const HitBuffer& buffer, uint32_t x, uint32_t y,
                                          const std::array<uint8_t, 3>& normalTexel, float tiling = 1.0f)
    {
        using namespace DirectX;
        const SurfaceHit& hit = *buffer.At(x, y);
        const SurfaceHit& right = *buffer.At(x + 1, y);
        const SurfaceHit& below = *buffer.At(x, y + 1);
        const XMVECTOR n = XMLoadFloat3(&hit.normal);
        const XMVECTOR p = XMLoadFloat3(&hit.position);
        const XMVECTOR dp1 = XMVectorSubtract(XMLoadFloat3(&right.position), p);
        const XMVECTOR dp2 = XMVectorSubtract(XMLoadFloat3(&below.position), p);
        const float du1 = (right.uv.x - hit.uv.x) * tiling;
        const float dv1 = (right.uv.y - hit.uv.y) * tiling;
        const float du2 = (below.uv.x - hit.uv.x) * tiling;
        const float dv2 = (below.uv.y - hit.uv.y) * tiling;
        const XMVECTOR dp2perp = XMVector3Cross(dp2, n);
        const XMVECTOR dp1perp = XMVector3Cross(n, dp1);
        const XMVECTOR t = XMVectorAdd(XMVectorScale(dp2perp, du1), XMVectorScale(dp1perp, du2));
        const XMVECTOR b = XMVectorAdd(XMVectorScale(dp2perp, dv1), XMVectorScale(dp1perp, dv2));
        const float maxLen2 = std::max(XMVectorGetX(XMVector3Dot(t, t)), XMVectorGetX(XMVector3Dot(b, b)));
        const float invMax = maxLen2 > 1.0e-20f ? 1.0f / std::sqrt(maxLen2) : 0.0f;
        const float nx = normalTexel[0] / 255.0f * 2.0f - 1.0f;
        const float ny = normalTexel[1] / 255.0f * 2.0f - 1.0f;
        const float nz = normalTexel[2] / 255.0f * 2.0f - 1.0f;
        const XMVECTOR mapped = XMVectorAdd(XMVectorAdd(XMVectorScale(t, nx * invMax), XMVectorScale(b, ny * invMax)),
                                            XMVectorScale(n, nz));
        XMFLOAT3 out;
        XMStoreFloat3(&out, XMVector3Normalize(mapped));
        return out;
    }

    /// Largest texel footprint of one pixel step, for a texture of @p texels per UV unit.
    inline float TexelFootprint(const HitBuffer& buffer, uint32_t x, uint32_t y, float texels)
    {
        const SurfaceHit& hit = *buffer.At(x, y);
        const SurfaceHit& right = *buffer.At(x + 1, y);
        const SurfaceHit& below = *buffer.At(x, y + 1);
        return std::max(std::hypot(right.uv.x - hit.uv.x, right.uv.y - hit.uv.y),
                        std::hypot(below.uv.x - hit.uv.x, below.uv.y - hit.uv.y)) *
               texels;
    }

    // ------------------------------------------------------------------------
    // Whole-frame probing
    // ------------------------------------------------------------------------

    struct ProbeTally
    {
        int background = 0;
        int surface = 0;
        int failures = 0;
    };

    /// The expected colour of an interior pixel, or nothing to skip it.
    using SurfaceReference = std::function<std::optional<Color3>(const SurfaceHit& hit, uint32_t x, uint32_t y)>;

    /// Writes a probed frame's CPU reference as <scene>_cpu.png (magenta where no
    /// probe applies) when SPARK_GOLDEN_WRITE_ACTUAL=1.
    inline void WriteReferenceImage(const char* scene, const std::vector<uint8_t>& reference, uint32_t width,
                                    uint32_t height)
    {
        if (!WriteActualRequested())
        {
            return;
        }
        const std::filesystem::path path = OutputDir() / (std::string(scene) + "_cpu.png");
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        Spark::GoldenImageTestRunner::SavePNG(path.string(), reference.data(), width, height);
    }

    /**
     * @brief Compares every interior or background pixel with the CPU reference.
     */
    inline ProbeTally ProbeFrame(const char* scene, const std::vector<uint8_t>& rgba, const HitBuffer& buffer,
                                 const Color3& clear, const SurfaceReference& reference, int tolerance = 2)
    {
        ProbeTally tally;
        if (rgba.size() != size_t(buffer.width) * buffer.height * 4)
        {
            tally.failures = 1;
            std::printf("[RHI-210 GOLDEN] %s readback/reference dimensions disagree\n", scene);
            return tally;
        }
        std::vector<uint8_t> expectedImage(size_t(buffer.width) * buffer.height * 4, 255);
        for (size_t i = 0; i < expectedImage.size(); i += 4)
        {
            expectedImage[i + 1] = 0; // magenta: not probed
        }
        for (uint32_t y = 0; y < buffer.height; ++y)
        {
            for (uint32_t x = 0; x < buffer.width; ++x)
            {
                const PixelClass pixelClass = Classify(buffer, x, y);
                std::optional<Color3> expected;
                if (pixelClass == PixelClass::Background)
                {
                    expected = clear;
                    ++tally.background;
                }
                else if (pixelClass == PixelClass::Interior)
                {
                    expected = reference(*buffer.At(x, y), x, y);
                    tally.surface += expected ? 1 : 0;
                }
                if (!expected)
                {
                    continue;
                }
                uint8_t* out = &expectedImage[(size_t(y) * buffer.width + x) * 4];
                out[0] = static_cast<uint8_t>(ToUnorm8((*expected)[0]));
                out[1] = static_cast<uint8_t>(ToUnorm8((*expected)[1]));
                out[2] = static_cast<uint8_t>(ToUnorm8((*expected)[2]));
                const auto pixel = PixelAt(rgba, buffer.width, x, y);
                if (ChannelError(pixel, *expected) > tolerance)
                {
                    if (tally.failures < 8)
                    {
                        std::printf("[RHI-210 GOLDEN] %s probe (%u,%u) = (%d,%d,%d), expected (%d,%d,%d) +/-%d\n",
                                    scene, x, y, pixel[0], pixel[1], pixel[2], ToUnorm8((*expected)[0]),
                                    ToUnorm8((*expected)[1]), ToUnorm8((*expected)[2]), tolerance);
                    }
                    ++tally.failures;
                }
            }
        }
        std::printf("[RHI-210 GOLDEN] %s CPU probes: %d background, %d surface, %d outside tolerance\n", scene,
                    tally.background, tally.surface, tally.failures);
        WriteReferenceImage(scene, expectedImage, buffer.width, buffer.height);
        return tally;
    }
} // namespace RHI210Golden

#endif // _WIN32
