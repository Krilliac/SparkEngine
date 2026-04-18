/**
 * @file RTSceneFeeder.cpp
 * @brief ECS walker that populates HybridRTManager's triangle-mesh scene.
 *
 * See RTSceneFeeder.h for the contract. Implementation notes:
 *   - Iterates `Transform + MeshRenderer` exactly the same way
 *     `RenderSystem::Update` does (ECSystems.cpp:51) so the traversal
 *     order matches the draw order.
 *   - Resolves `MeshRenderer::meshPath` via `AssetPipeline::LoadMesh`,
 *     which is a hashed cache lookup in steady state — no allocation
 *     once meshes have been loaded once.
 *   - Uses `renderer.cachedWorldMatrix` when valid (populated by
 *     RenderSystem earlier in the frame); otherwise falls back to
 *     `Transform::GetWorldMatrix(registry)` which walks parent hierarchy.
 *   - `renderer.castShadows == false` still gets pushed — RT sees the
 *     mesh for reflections/GI even if it doesn't cast shadows in the
 *     shadow-map pass. Visibility (`renderer.visible`) IS honored.
 *   - Entities with `ActiveComponent::active == false` are skipped.
 */

#include "RTSceneFeeder.h"
#include "HybridRTManager.h"

#include "../AssetPipeline.h"
#include "../../Core/Platform.h"
#include "../../Engine/ECS/Components.h"
#include "../../Engine/ECS/Components/CoreComponents.h"
#include "../../Utils/Logger.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

namespace Spark::Graphics
{
    uint32_t PopulateRTSceneFromECS(HybridRTManager& rt, World& world, AssetPipeline& assets)
    {
        // Start from a clean slate each call — BLAS handles are stable
        // only for the duration of a Push/Clear cycle, and transforms
        // may have changed since the last sync.
        rt.ClearTriangleMeshes();

        const auto& registry = world.GetRegistry();
        auto view = world.GetEntitiesWith<Transform, MeshRenderer>();

        uint32_t pushedCount = 0;
        for (auto entity : view)
        {
            auto& transform = view.get<Transform>(entity);
            auto& renderer = view.get<MeshRenderer>(entity);

            if (!renderer.visible)
                continue;

            auto* active = registry.try_get<ActiveComponent>(entity);
            if (active && !active->active)
                continue;

            if (renderer.meshPath.empty())
                continue;

            auto meshAsset = assets.LoadMesh(renderer.meshPath);
            if (!meshAsset)
                continue;

            DirectX::XMMATRIX worldMtx = renderer.worldMatrixDirty
                                             ? transform.GetWorldMatrix(registry)
                                             : DirectX::XMLoadFloat4x4(&renderer.cachedWorldMatrix);

            rt.PushTriangleMesh(*meshAsset, worldMtx, /*allowDynamicUpdate*/ false);
            ++pushedCount;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RTSceneFeeder: pushed %u meshes into HybridRTManager",
                       pushedCount);
        return pushedCount;
    }
} // namespace Spark::Graphics
