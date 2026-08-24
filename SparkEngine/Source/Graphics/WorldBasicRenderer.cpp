#include "Graphics/WorldBasicRenderer.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Game/PlaceholderMesh.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"

#include <cmath>
#include <vector>

using namespace DirectX;

namespace Spark
{

    namespace
    {
        HRESULT CreateCylinderMesh(Mesh& mesh)
        {
            constexpr unsigned int segments = 24;
            constexpr float radius = 0.5f;
            constexpr float halfHeight = 0.5f;
            constexpr float twoPi = 6.28318530717958647692f;

            std::vector<Vertex> vertices;
            std::vector<unsigned int> indices;
            vertices.reserve(segments * 2 + 2);
            indices.reserve(segments * 12);

            for (unsigned int i = 0; i < segments; ++i)
            {
                const float angle = twoPi * static_cast<float>(i) / static_cast<float>(segments);
                const float x = radius * std::cos(angle);
                const float z = radius * std::sin(angle);
                const float u = static_cast<float>(i) / static_cast<float>(segments);
                vertices.emplace_back(XMFLOAT3{x, -halfHeight, z}, XMFLOAT3{x / radius, 0.0f, z / radius},
                                      XMFLOAT2{u, 1.0f});
                vertices.emplace_back(XMFLOAT3{x, halfHeight, z}, XMFLOAT3{x / radius, 0.0f, z / radius},
                                      XMFLOAT2{u, 0.0f});
            }
            const unsigned int bottomCenter = static_cast<unsigned int>(vertices.size());
            vertices.emplace_back(XMFLOAT3{0.0f, -halfHeight, 0.0f}, XMFLOAT3{0.0f, -1.0f, 0.0f},
                                  XMFLOAT2{0.5f, 0.5f});
            const unsigned int topCenter = static_cast<unsigned int>(vertices.size());
            vertices.emplace_back(XMFLOAT3{0.0f, halfHeight, 0.0f}, XMFLOAT3{0.0f, 1.0f, 0.0f},
                                  XMFLOAT2{0.5f, 0.5f});

            for (unsigned int i = 0; i < segments; ++i)
            {
                const unsigned int next = (i + 1) % segments;
                const unsigned int b0 = i * 2;
                const unsigned int t0 = b0 + 1;
                const unsigned int b1 = next * 2;
                const unsigned int t1 = b1 + 1;

                indices.insert(indices.end(), {b0, t0, t1, b0, t1, b1});
                indices.insert(indices.end(), {bottomCenter, b1, b0});
                indices.insert(indices.end(), {topCenter, t0, t1});
            }
            return mesh.CreateFromVertices(vertices, indices);
        }

        bool CreateReservedPrimitive(Mesh& mesh, GraphicsEngine& graphics, const std::string& path)
        {
            const bool isCube = path == "__spark_primitive_Cube.obj";
            const bool isSphere = path == "__spark_primitive_Sphere.obj";
            const bool isCylinder = path == "__spark_primitive_Cylinder.obj";
            const bool isPlane = path == "__spark_primitive_Plane.obj" || path == "__spark_primitive_ground__.obj" ||
                                 path == "__spark_primitive_sprite__.obj";
            if (!isCube && !isSphere && !isCylinder && !isPlane)
                return false;

            const HRESULT initialized = mesh.Initialize(graphics.GetDevice(), graphics.GetContext());
            if (FAILED(initialized))
                return true;

            HRESULT result = E_INVALIDARG;
            if (isCube)
                result = mesh.CreateCube(1.0f);
            else if (isSphere)
                result = mesh.CreateSphere(0.5f, 24, 16);
            else if (isCylinder)
                result = CreateCylinderMesh(mesh);
            else if (isPlane)
                result = mesh.CreatePlane(1.0f, 1.0f);

            if (FAILED(result))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                               "Reserved primitive '%s' failed to generate; using cube fallback", path.c_str());
                mesh.CreateCube(1.0f);
                mesh.SetPlaceholder(true);
            }
            return true;
        }
    } // namespace

    WorldMeshCache::WorldMeshCache() = default;
    WorldMeshCache::~WorldMeshCache() = default;

    Mesh* WorldMeshCache::GetOrLoad(GraphicsEngine& g, const std::string& path)
    {
        if (path.empty() || !g.GetDevice())
            return nullptr;
        if (auto it = m_cache.find(path); it != m_cache.end())
            return it->second.get();
        auto mesh = std::make_unique<Mesh>();
        if (!CreateReservedPrimitive(*mesh, g, path))
            LoadOrPlaceholderMesh(*mesh, g.GetDevice(), g.GetContext(), std::wstring(path.begin(), path.end()));
        Mesh* raw = mesh.get();
        m_cache.emplace(path, std::move(mesh));
        return raw;
    }

    WorldBasicRenderStats RenderWorldBasic(World& world, GraphicsEngine& g, WorldMeshCache& cache,
                                           const XMMATRIX& view, const XMMATRIX& proj)
    {
        WorldBasicRenderStats stats;
        g.SetBasicShaders();
        g.ApplyBasicRenderStates();
        // Set the per-frame lighting/camera constant buffer that the basic pixel
        // shader reads (directional + ambient + camera-facing fill). Without this,
        // callers that don't run GraphicsEngine's normal BeginFrame path (the -scene
        // runtime and the editor viewport) leave the lighting cbuffer zeroed, so
        // everything renders black. Camera position is the inverse-view translation.
        XMVECTOR det;
        const XMMATRIX invView = XMMatrixInverse(&det, view);
        XMFLOAT3 camPos;
        XMStoreFloat3(&camPos, invView.r[3]);
        g.UpdateFrameConstants(view, proj, camPos);
        for (auto e : world.GetEntitiesWith<Transform, MeshRenderer>())
        {
            ++stats.candidates;
            const MeshRenderer* mr = world.GetComponent<MeshRenderer>(e);
            if (!mr->visible)
                continue;
            ++stats.visible;
            Mesh* mesh = cache.GetOrLoad(g, mr->meshPath);
            if (!mesh || mesh->GetIndexCount() == 0)
                continue;
            const Transform* t = world.GetComponent<Transform>(e);
            const XMMATRIX wmat = t->GetWorldMatrix(world.GetRegistry());
            ID3D11ShaderResourceView* srv = nullptr;
            if (!mr->materialPath.empty())
            {
                if (mr->materialPath.size() > 5 &&
                    mr->materialPath.compare(mr->materialPath.size() - 5, 5, ".json") == 0)
                {
                    if (const auto* mat = g.GetOrLoadBasicMaterial(mr->materialPath))
                        srv = mat->srv.Get();
                }
                else
                {
                    srv = g.GetOrLoadTextureSRV(mr->materialPath);
                }
            }
            g.UpdateBasicConstants(wmat, view, proj, XMFLOAT4(1, 1, 1, 1), XMFLOAT2(1, 1));
            g.SetBasicTexture(srv);
            mesh->Render(g.GetContext());
            ++stats.drawn;
        }

        // The editor's Create Sprite surface produces SpriteRenderer entities,
        // while both editor viewports use this lightweight World renderer.
        // Render those entities as camera-facing XY quads so creation is not a
        // silent/invisible operation. The full runtime SpriteBatch remains the
        // path for atlas batching and layer sorting.
        for (auto e : world.GetEntitiesWith<Transform, SpriteRenderer>())
        {
            ++stats.candidates;
            const SpriteRenderer* sprite = world.GetComponent<SpriteRenderer>(e);
            if (!sprite->visible)
                continue;
            ++stats.visible;

            Mesh* quad = cache.GetOrLoad(g, "__spark_primitive_sprite__.obj");
            if (!quad || quad->GetIndexCount() == 0)
                continue;

            XMFLOAT2 size = sprite->GetWorldSize();
            if (size.x <= 0.0f)
                size.x = 1.0f;
            if (size.y <= 0.0f)
                size.y = 1.0f;
            if (sprite->flipX)
                size.x = -size.x;
            if (sprite->flipY)
                size.y = -size.y;

            const Transform* transform = world.GetComponent<Transform>(e);
            const XMMATRIX spriteLocal = XMMatrixScaling(size.x, 1.0f, size.y) * XMMatrixRotationX(XM_PIDIV2);
            const XMMATRIX worldMatrix = spriteLocal * transform->GetWorldMatrix(world.GetRegistry());
            ID3D11ShaderResourceView* texture = sprite->texturePath.empty()
                                                    ? nullptr
                                                    : g.GetOrLoadTextureSRV(sprite->texturePath);
            g.UpdateBasicConstants(worldMatrix, view, proj, sprite->color, XMFLOAT2(1, 1));
            g.SetBasicTexture(texture);
            quad->Render(g.GetContext());
            ++stats.drawn;
        }
        g.SetBasicTexture(nullptr);
        return stats;
    }

} // namespace Spark
