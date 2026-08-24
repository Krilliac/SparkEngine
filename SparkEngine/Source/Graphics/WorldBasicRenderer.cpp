#include "Graphics/WorldBasicRenderer.h"
#include "Engine/ECS/Components.h"
#include "Engine/ECS/Components/CoreComponents.h"
#include "Game/PlaceholderMesh.h"
#include "Graphics/GraphicsEngine.h"
#include "Graphics/Mesh.h"
#include "Graphics/ProjectAssetPath.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <type_traits>
#include <vector>

using namespace DirectX;

namespace Spark
{
    namespace
    {
        std::string MeshFileSignature(const std::filesystem::path& path)
        {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(path, ec) || ec)
                return "missing";

            const auto size = std::filesystem::file_size(path, ec);
            if (ec)
                return "unreadable";
            const auto modified = std::filesystem::last_write_time(path, ec);
            if (ec)
                return "unreadable";
            return std::to_string(size) + ":" + std::to_string(modified.time_since_epoch().count());
        }
    } // namespace


    namespace
    {
        struct ResolvedSpriteDraw
        {
            EntityID entity = entt::null;
            const Transform* transform = nullptr;
            const SpriteRenderer* sprite = nullptr;
            XMFLOAT2 uvScale{1.0f, 1.0f};
            XMFLOAT2 uvOffset{0.0f, 0.0f};
            XMFLOAT2 size{1.0f, 1.0f};
            XMFLOAT2 pivot{0.5f, 0.5f};
        };

        bool ResolveSpriteDraw(EntityID entity, const Transform& transform, const SpriteRenderer& sprite,
                               ResolvedSpriteDraw& out)
        {
            const XMFLOAT4& input = sprite.sourceRect;
            if (!std::isfinite(input.x) || !std::isfinite(input.y) || !std::isfinite(input.z) ||
                !std::isfinite(input.w))
            {
                return false;
            }

            const float u0 = std::clamp(input.x, 0.0f, 1.0f);
            const float v0 = std::clamp(input.y, 0.0f, 1.0f);
            const float u1 = std::clamp(input.z, 0.0f, 1.0f);
            const float v1 = std::clamp(input.w, 0.0f, 1.0f);
            if (u1 <= u0 || v1 <= v0)
                return false;

            const float uvWidth = u1 - u0;
            const float uvHeight = v1 - v0;
            out.entity = entity;
            out.transform = &transform;
            out.sprite = &sprite;
            // CreatePlane's +Z edge carries v=0 and becomes the sprite's top
            // after the +90 degree X rotation below. The unflipped draw must
            // therefore walk V from v1 at the bottom to v0 at the top.
            out.uvScale = {sprite.flipX ? -uvWidth : uvWidth, sprite.flipY ? uvHeight : -uvHeight};
            out.uvOffset = {sprite.flipX ? u1 : u0, sprite.flipY ? v0 : v1};

            // Unknown source dimensions are common while an editor asset is
            // still importing. Preserve the historical one-unit preview in
            // that case, but use the clamped source rectangle whenever enough
            // information is available to compute the real world size.
            if (sprite.pixelsPerUnit > 0.0f && std::isfinite(sprite.pixelsPerUnit))
            {
                if (sprite.textureWidth > 0)
                    out.size.x = static_cast<float>(sprite.textureWidth) * uvWidth / sprite.pixelsPerUnit;
                if (sprite.textureHeight > 0)
                    out.size.y = static_cast<float>(sprite.textureHeight) * uvHeight / sprite.pixelsPerUnit;
            }
            if (!std::isfinite(out.size.x) || out.size.x <= 0.0f)
                out.size.x = 1.0f;
            if (!std::isfinite(out.size.y) || out.size.y <= 0.0f)
                out.size.y = 1.0f;

            out.pivot.x = std::isfinite(sprite.pivot.x) ? std::clamp(sprite.pivot.x, 0.0f, 1.0f) : 0.5f;
            out.pivot.y = std::isfinite(sprite.pivot.y) ? std::clamp(sprite.pivot.y, 0.0f, 1.0f) : 0.5f;
            return true;
        }

        auto EntitySortKey(EntityID entity)
        {
            return entt::to_entity(entity);
        }

#ifdef SPARK_PLATFORM_WINDOWS
        class ScopedSpriteOutputMergerState
        {
          public:
            explicit ScopedSpriteOutputMergerState(ID3D11DeviceContext* context) : m_context(context)
            {
                if (!m_context)
                    return;
                m_context->OMGetBlendState(m_blendState.GetAddressOf(), m_blendFactor, &m_sampleMask);
                m_context->OMGetDepthStencilState(m_depthState.GetAddressOf(), &m_stencilRef);
            }

            ~ScopedSpriteOutputMergerState()
            {
                if (!m_context)
                    return;
                m_context->OMSetBlendState(m_blendState.Get(), m_blendFactor, m_sampleMask);
                m_context->OMSetDepthStencilState(m_depthState.Get(), m_stencilRef);
            }

            ScopedSpriteOutputMergerState(const ScopedSpriteOutputMergerState&) = delete;
            ScopedSpriteOutputMergerState& operator=(const ScopedSpriteOutputMergerState&) = delete;

          private:
            ID3D11DeviceContext* m_context = nullptr;
            Microsoft::WRL::ComPtr<ID3D11BlendState> m_blendState;
            Microsoft::WRL::ComPtr<ID3D11DepthStencilState> m_depthState;
            float m_blendFactor[4]{};
            UINT m_sampleMask = 0xffffffff;
            UINT m_stencilRef = 0;
        };
#endif

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
            vertices.emplace_back(XMFLOAT3{0.0f, -halfHeight, 0.0f}, XMFLOAT3{0.0f, -1.0f, 0.0f}, XMFLOAT2{0.5f, 0.5f});
            const unsigned int topCenter = static_cast<unsigned int>(vertices.size());
            vertices.emplace_back(XMFLOAT3{0.0f, halfHeight, 0.0f}, XMFLOAT3{0.0f, 1.0f, 0.0f}, XMFLOAT2{0.5f, 0.5f});

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

    std::optional<ResolvedProjectAssetPath> WorldMeshCache::ResolveAsset(std::string_view projectRootUtf8,
                                                                         const std::string& path)
    {
        const std::string declaredKey = std::string(projectRootUtf8) + '\n' + path;
        if (const auto found = m_resolvedAssets.find(declaredKey); found != m_resolvedAssets.end())
            return found->second;

        const auto resolved = ResolveProjectAssetPath(projectRootUtf8, path);
        if (resolved)
            m_resolvedAssets.emplace(declaredKey, *resolved);
        return resolved;
    }

    void WorldMeshCache::WarnRejectedAssetOnce(std::string_view kind, const std::string& path)
    {
        const std::string warningKey = std::string(kind) + '\n' + path;
        if (m_warnedRejectedAssets.emplace(warningKey).second)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics, "WorldBasicRenderer rejected %.*s '%s'",
                           static_cast<int>(kind.size()), kind.data(), path.c_str());
        }
    }

    Mesh* WorldMeshCache::GetOrLoad(GraphicsEngine& g, const std::string& path, std::string_view projectRootUtf8)
    {
        if (path.empty() || !g.GetDevice())
            return nullptr;

        if (path.rfind("__spark_primitive_", 0) == 0)
        {
            if (auto it = m_cache.find(path); it != m_cache.end())
                return it->second.get();
            auto mesh = std::make_unique<Mesh>();
            if (!CreateReservedPrimitive(*mesh, g, path))
                return nullptr;
            Mesh* raw = mesh.get();
            m_cache.emplace(path, std::move(mesh));
            return raw;
        }

        auto resolved = ResolveAsset(projectRootUtf8, path);
        if (!resolved)
        {
            WarnRejectedAssetOnce("mesh asset", path);
            return nullptr;
        }
        if (auto it = m_cache.find(resolved->cacheKey); it != m_cache.end())
        {
            if (!it->second->IsPlaceholder())
                return it->second.get();

            // Retry a failed/missing asset only when its filesystem state
            // changes. Existence-only retrying reopened corrupt OBJ and
            // unsupported files, rebuilt GPU fallback geometry, and logged a
            // warning every frame.
            const std::string currentSignature = MeshFileSignature(resolved->nativePath);
            const auto failed = m_placeholderFileSignatures.find(resolved->cacheKey);
            if (failed != m_placeholderFileSignatures.end() && failed->second == currentSignature)
                return it->second.get();

            // The missing/corrupt path may have been replaced through a
            // junction or symlink since its placeholder was cached. Re-run
            // canonical confinement immediately before any retry/open.
            const auto refreshed = ResolveProjectAssetPath(projectRootUtf8, path);
            if (!refreshed)
            {
                WarnRejectedAssetOnce("mesh asset after filesystem change", path);
                return it->second.get();
            }
            m_cache.erase(it);
            m_placeholderFileSignatures.erase(resolved->cacheKey);
            resolved = refreshed;
            m_resolvedAssets[std::string(projectRootUtf8) + '\n' + path] = *resolved;
            if (const auto refreshedCached = m_cache.find(resolved->cacheKey); refreshedCached != m_cache.end())
                return refreshedCached->second.get();
        }

        auto mesh = std::make_unique<Mesh>();
#ifdef SPARK_PLATFORM_WINDOWS
        const std::wstring nativePath = resolved->nativePath.native();
#else
        const std::string nativePathBytes = resolved->nativePath.string();
        const std::wstring nativePath(nativePathBytes.begin(), nativePathBytes.end());
#endif
        LoadOrPlaceholderMesh(*mesh, g.GetDevice(), g.GetContext(), nativePath);
        Mesh* raw = mesh.get();
        if (mesh->IsPlaceholder())
            m_placeholderFileSignatures[resolved->cacheKey] = MeshFileSignature(resolved->nativePath);
        else
            m_placeholderFileSignatures.erase(resolved->cacheKey);
        m_cache.emplace(resolved->cacheKey, std::move(mesh));
        return raw;
    }

    WorldBasicRenderStats RenderWorldBasic(World& world, GraphicsEngine& g, WorldMeshCache& cache, const XMMATRIX& view,
                                           const XMMATRIX& proj, std::string_view projectRootUtf8)
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
            const ActiveComponent* active = world.GetComponent<ActiveComponent>(e);
            if (!mr->visible || (active && !active->active))
                continue;
            ++stats.visible;
            Mesh* mesh = cache.GetOrLoad(g, mr->meshPath, projectRootUtf8);
            if (!mesh || mesh->GetIndexCount() == 0)
            {
                ++stats.rejected;
                continue;
            }
            const Transform* t = world.GetComponent<Transform>(e);
            const XMMATRIX wmat = t->GetWorldMatrix(world.GetRegistry());
            ID3D11ShaderResourceView* srv = nullptr;
            if (!mr->materialPath.empty())
            {
                const auto materialPath = cache.ResolveAsset(projectRootUtf8, mr->materialPath);
                if (materialPath && materialPath->nativePath.extension() == ".json")
                {
                    if (const auto* mat = g.GetOrLoadBasicMaterial(mr->materialPath, projectRootUtf8))
                        srv = mat->srv.Get();
                }
                else if (materialPath)
                {
                    srv = g.GetOrLoadTextureSRV(materialPath->cacheKey);
                }
                else
                {
                    ++stats.rejected;
                    cache.WarnRejectedAssetOnce("material asset", mr->materialPath);
                }
            }
            g.UpdateBasicConstants(wmat, view, proj, XMFLOAT4(1, 1, 1, 1), XMFLOAT2(1, 1));
            g.SetBasicTexture(srv);
            mesh->Render(g.GetContext());
            ++stats.drawn;
        }

        // The editor's Create Sprite surface produces SpriteRenderer entities,
        // while both editor viewports use this lightweight World renderer.
        // Resolve and sort the full sprite batch before drawing: higher layers
        // and orders are submitted later, with entity identity as the stable
        // final tie-breaker independent of EnTT storage iteration order.
        std::vector<ResolvedSpriteDraw> spriteDraws;
        for (auto e : world.GetEntitiesWith<Transform, SpriteRenderer>())
        {
            ++stats.candidates;
            const SpriteRenderer* sprite = world.GetComponent<SpriteRenderer>(e);
            const ActiveComponent* active = world.GetComponent<ActiveComponent>(e);
            if (!sprite->visible || (active && !active->active))
                continue;
            ++stats.visible;
            const Transform* transform = world.GetComponent<Transform>(e);
            ResolvedSpriteDraw resolved;
            if (ResolveSpriteDraw(e, *transform, *sprite, resolved))
                spriteDraws.push_back(resolved);
            else
            {
                ++stats.rejected;
                cache.WarnRejectedAssetOnce("sprite source rectangle", std::to_string(EntitySortKey(e)));
            }
        }

        std::sort(spriteDraws.begin(), spriteDraws.end(),
                  [](const ResolvedSpriteDraw& a, const ResolvedSpriteDraw& b)
                  {
                      return std::tuple{a.sprite->sortingLayer, a.sprite->orderInLayer, EntitySortKey(a.entity)} <
                             std::tuple{b.sprite->sortingLayer, b.sprite->orderInLayer, EntitySortKey(b.entity)};
                  });

        Mesh* quad = spriteDraws.empty() ? nullptr : cache.GetOrLoad(g, "__spark_primitive_sprite__.obj", {});
        if (quad && quad->GetIndexCount() != 0)
        {
#ifdef SPARK_PLATFORM_WINDOWS
            ScopedSpriteOutputMergerState restoreOutputMergerState(g.GetContext());
            g.SetBasicBlendMode(GraphicsEngine::BasicBlendMode::Alpha);
            g.SetBasicDepthMode(GraphicsEngine::BasicDepthMode::ReadOnly);
#endif
            for (const ResolvedSpriteDraw& draw : spriteDraws)
            {
                const float pivotOffsetX = (0.5f - draw.pivot.x) * draw.size.x;
                const float pivotOffsetY = (draw.pivot.y - 0.5f) * draw.size.y;
                const XMMATRIX spriteLocal = XMMatrixScaling(draw.size.x, 1.0f, draw.size.y) *
                                             XMMatrixRotationX(XM_PIDIV2) *
                                             XMMatrixTranslation(pivotOffsetX, pivotOffsetY, 0.0f);
                const XMMATRIX worldMatrix = spriteLocal * draw.transform->GetWorldMatrix(world.GetRegistry());
                const SpriteRenderer* sprite = draw.sprite;
                ID3D11ShaderResourceView* texture = nullptr;
                if (!sprite->texturePath.empty())
                {
                    if (const auto texturePath = cache.ResolveAsset(projectRootUtf8, sprite->texturePath))
                        texture = g.GetOrLoadTextureSRV(texturePath->cacheKey);
                    else
                    {
                        ++stats.rejected;
                        cache.WarnRejectedAssetOnce("sprite texture", sprite->texturePath);
                    }
                }
                g.UpdateBasicConstants(worldMatrix, view, proj, sprite->color, draw.uvScale, 0.0f, 1.0f, draw.uvOffset);
                g.SetBasicTexture(texture);
                quad->Render(g.GetContext());
                ++stats.drawn;
            }
        }
        g.SetBasicTexture(nullptr);
        return stats;
    }

} // namespace Spark
