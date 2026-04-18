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

#include "../../Core/Platform.h"
#include "../../Engine/ECS/Components.h"
#include "../../Engine/ECS/Components/CoreComponents.h"
#include "../../Utils/Logger.h"
#include "../AssetPipeline.h"

#ifdef SPARK_PLATFORM_MACOS
#include "../MaterialSystem.h"
#include "RTMaterialAdapter.h"
#endif

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

#ifdef SPARK_PLATFORM_MACOS
    uint32_t PopulateRTMaterialsFromECS(HybridRTManager& rt, World& world, MaterialSystem& materials)
    {
        // Mirror the traversal in PopulateRTSceneFromECS exactly — the
        // MaterialParams[] index is the instance_id the Metal RT kernels
        // use, which is assigned in push order. Any divergence between
        // the two traversals would desync the material buffer from the
        // TLAS.
        const auto& registry = world.GetRegistry();
        auto view = world.GetEntitiesWith<Transform, MeshRenderer>();

        std::vector<Spark::RHI::Metal::MaterialParams> out;
        out.reserve(32);

        for (auto entity : view)
        {
            auto& renderer = view.get<MeshRenderer>(entity);
            if (!renderer.visible)
                continue;
            auto* active = registry.try_get<ActiveComponent>(entity);
            if (active && !active->active)
                continue;
            if (renderer.meshPath.empty())
                continue;

            // Resolve the material through MaterialSystem. If the
            // lookup misses, fall back to a neutral default — keeping
            // the array size aligned with the instance count is more
            // important than perfect shading for a missing asset.
            std::shared_ptr<Material> mat;
            if (!renderer.materialPath.empty())
                mat = materials.GetMaterial(renderer.materialPath);
            if (mat)
                out.push_back(MaterialParamsFromPBR(mat->GetPBRProperties()));
            else
                out.push_back(Spark::RHI::Metal::MaterialParams{});
        }

        rt.SetMetalMaterials(out);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RTSceneFeeder: uploaded %u materials into HybridRTManager",
                       static_cast<uint32_t>(out.size()));
        return static_cast<uint32_t>(out.size());
    }
#endif

} // namespace Spark::Graphics
