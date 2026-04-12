/**
 * @file FoliageRenderer.cpp
 * @brief CPU-portable foliage collection, batching, and LOD selection
 *
 * Batch construction walks every FoliageManager volume. For each visible
 * instance it:
 *   1. Resolves the species record by name via the volume descriptor.
 *   2. Loads-or-caches the mesh handle via the user-supplied loader.
 *   3. Computes a deterministic wind phase from the instance position.
 *   4. Builds a TRS world matrix and chooses mesh vs impostor LOD.
 *
 * The CPU path is entirely platform-agnostic so that
 * `Tests/TestFoliageRenderer.cpp` can run under GCC/Clang on Linux CI
 * without any D3D11 device. GPU-side upload, shader compilation, and
 * draw calls live in FoliageRendererGPU.cpp (Windows only).
 */

#include "FoliageRenderer.h"

#include "../Core/EngineContext.h"
#include "../Utils/Validate.h"
#include "AssetPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>
#include <unordered_set>
#include <utility>

// GPU-only includes live in FoliageRendererGPU.cpp
#ifdef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngine.h"
#endif

namespace Spark::Graphics
{

    namespace
    {
        constexpr float TWO_PI = 6.283185307179586f;
    }

    FoliageRenderer::FoliageRenderer() = default;
    FoliageRenderer::~FoliageRenderer() = default;

    FoliageRenderer& FoliageRenderer::GetInstance()
    {
        static FoliageRenderer s;
        return s;
    }

    // ========================================================================
    // Lifecycle
    // ========================================================================

    bool FoliageRenderer::Initialize(FoliageMeshLoader loader, float impostorDistance)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);

        m_impostorDistance = std::max(0.0f, impostorDistance);
        m_meshCache.clear();
        m_renderInstances.clear();
        m_stats = {};
        m_hasExplicitLoader = false;

        if (loader)
        {
            m_loader = std::move(loader);
            m_hasExplicitLoader = true;
        }
        else
        {
            // Placeholder that returns nullptr until a real loader is
            // installed (either explicitly via InstallAssetPipelineLoader
            // or lazily via TryInstallEngineContextLoader on first Collect).
            m_loader = [](const std::string&) -> std::shared_ptr<MeshAsset> { return nullptr; };
        }

        m_initialized = true;

        // Attempt the lazy path immediately so typical lifecycle flows
        // finish Initialize with a fully wired loader.
        if (!m_hasExplicitLoader)
        {
            TryInstallEngineContextLoader();
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "FoliageRenderer initialized (impostorDistance=%.1f, explicitLoader=%s)", m_impostorDistance,
                       m_hasExplicitLoader ? "yes" : "no");
        return true;
    }

    bool FoliageRenderer::InstallAssetPipelineLoader(::AssetPipeline* pipeline)
    {
        if (!pipeline)
            return false;

        m_loader = [pipeline](const std::string& path) -> std::shared_ptr<MeshAsset>
        {
            if (path.empty())
                return nullptr;
            return pipeline->LoadMesh(path);
        };
        m_hasExplicitLoader = true;

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FoliageRenderer: AssetPipeline-backed mesh loader installed");
        return true;
    }

    void FoliageRenderer::TryInstallEngineContextLoader()
    {
        if (m_hasExplicitLoader)
            return;

        auto* ctx = EngineContext::Get();
        if (!ctx)
            return;

        ::AssetPipeline* pipeline = ctx->GetAssetPipeline();
        if (!pipeline)
            return;

        InstallAssetPipelineLoader(pipeline);
    }

    void FoliageRenderer::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        const size_t cached = m_meshCache.size();
        m_meshCache.clear();
        m_renderInstances.clear();
        m_drawOrder.clear();
        m_stats = {};
        m_loader = nullptr;
        m_hasExplicitLoader = false;
        m_initialized = false;
#ifdef SPARK_PLATFORM_WINDOWS
        m_albedoCache.clear();
#endif
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FoliageRenderer shutting down (%zu meshes cached)", cached);
    }

    // ========================================================================
    // Mesh cache
    // ========================================================================

    std::shared_ptr<MeshAsset> FoliageRenderer::GetOrLoadMesh(const std::string& path)
    {
        if (path.empty())
            return nullptr;

        auto it = m_meshCache.find(path);
        if (it != m_meshCache.end())
        {
            ++m_stats.meshCacheHits;
            return it->second;
        }

        ++m_stats.meshCacheMisses;
        std::shared_ptr<MeshAsset> loaded = m_loader ? m_loader(path) : nullptr;
        m_meshCache.emplace(path, loaded);
        return loaded;
    }

    void FoliageRenderer::PrewarmMesh(const std::string& path, std::shared_ptr<MeshAsset> mesh)
    {
        if (path.empty())
            return;
        m_meshCache[path] = std::move(mesh);
    }

    // ========================================================================
    // Pure helpers
    // ========================================================================

    void FoliageRenderer::BuildWorldMatrix(const FoliageInstance& instance, float* out16)
    {
        if (!out16)
            return;

        const float cy = std::cos(instance.yawRadians);
        const float sy = std::sin(instance.yawRadians);
        const float s = instance.scale;

        // Row-major TRS:
        // [ s*cy   0   s*sy   0 ]
        // [ 0      s   0      0 ]
        // [-s*sy   0   s*cy   0 ]
        // [ px    py   pz     1 ]
        out16[0] = s * cy;
        out16[1] = 0.0f;
        out16[2] = s * sy;
        out16[3] = 0.0f;

        out16[4] = 0.0f;
        out16[5] = s;
        out16[6] = 0.0f;
        out16[7] = 0.0f;

        out16[8] = -s * sy;
        out16[9] = 0.0f;
        out16[10] = s * cy;
        out16[11] = 0.0f;

        out16[12] = instance.position.x;
        out16[13] = instance.position.y;
        out16[14] = instance.position.z;
        out16[15] = 1.0f;
    }

    float FoliageRenderer::ComputeWindPhase(const FoliageInstance& instance)
    {
        // Quantise to 10 cm cells so nearby instances get similar phases,
        // but distant ones decorrelate — prevents the whole field from
        // swaying in unison.
        const int32_t qx = static_cast<int32_t>(std::floor(instance.position.x * 10.0f));
        const int32_t qy = static_cast<int32_t>(std::floor(instance.position.y * 10.0f));
        const int32_t qz = static_cast<int32_t>(std::floor(instance.position.z * 10.0f));

        // Knuth multiplicative hash — cheap, well-distributed.
        uint32_t h = static_cast<uint32_t>(qx) * 2654435761u;
        h ^= static_cast<uint32_t>(qz) * 2246822519u;
        h ^= static_cast<uint32_t>(qy) * 3266489917u;
        h ^= h >> 16;

        const float norm = static_cast<float>(h) * (1.0f / 4294967295.0f);
        return norm * TWO_PI;
    }

    FoliageRenderLOD FoliageRenderer::SelectLOD(float distanceToCamera, float impostorDistance)
    {
        return (distanceToCamera <= impostorDistance) ? FoliageRenderLOD::Mesh : FoliageRenderLOD::Impostor;
    }

    void FoliageRenderer::BuildGPUInstance(const FoliageRenderInstance& src, GPUInstanceData& out)
    {
        std::memcpy(&out.worldMatrix, src.worldMatrix, sizeof(float) * 16);
        // Default prev matrix to current — first-frame motion vectors will
        // be zero until GPUSceneBuffer::UpdateTransform is used to roll the
        // previous frame forward.
        std::memcpy(&out.prevWorldMatrix, src.worldMatrix, sizeof(float) * 16);

        // normalMatrix = inverse-transpose(world) for correct foliage
        // normals under non-uniform wind-driven scale.
        DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&out.worldMatrix);
        DirectX::XMMATRIX normal = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, world));
        DirectX::XMStoreFloat4x4(&out.normalMatrix, normal);

        // Use the registry-wide global index; `speciesIndex` alone is
        // volume-local and would alias different species across volumes.
        out.materialId = src.globalMaterialId;
        out.flags = static_cast<uint32_t>(InstanceFlags::Visible) | static_cast<uint32_t>(InstanceFlags::CastShadow) |
                    static_cast<uint32_t>(InstanceFlags::ReceiveShadow);
        if (src.lod == FoliageRenderLOD::Impostor)
        {
            SetFoliageImpostorFlag(out, true);
        }
        out.lodDistance = src.distanceToCamera;
        // Pack the wind phase into the padding slot so the foliage vertex
        // shader can read it alongside the world matrix without growing
        // GPUInstanceData.
        out.padding = src.windPhase;
    }

    void FoliageRenderer::ClampDrawRunToUploaded(const FoliageDrawRun& run, uint32_t uploadedCount,
                                                 uint32_t& outStartInstance, uint32_t& outInstanceCount)
    {
        outStartInstance = run.startInstance;
        if (run.startInstance >= uploadedCount)
        {
            // Entire run is past the uploaded tail — caller must skip.
            outInstanceCount = 0;
            return;
        }
        const uint32_t available = uploadedCount - run.startInstance;
        outInstanceCount = std::min(run.instanceCount, available);
    }

    // ========================================================================
    // Per-frame collection
    // ========================================================================

    void FoliageRenderer::CollectFromFoliageManager(float /*windTime*/)
    {
        m_renderInstances.clear();
        m_drawOrder.clear();
        m_stats = {};

        if (!m_initialized)
            return;

        // Lazy loader install: the engine lifecycle calls Initialize()
        // before AssetPipeline is guaranteed to be registered with
        // EngineContext. Retry the lookup each frame until a real loader
        // is in place; after that it is a single bool check.
        if (!m_hasExplicitLoader)
        {
            TryInstallEngineContextLoader();
        }

        auto& manager = FoliageManager::GetInstance();
        if (!manager.IsInitialized())
            return;

        std::unordered_set<uint64_t> uniqueKeys;

        for (uint32_t volId : manager.GetVolumeIds())
        {
            const FoliageVolumeDesc* desc = manager.GetVolumeDesc(volId);
            if (!desc)
                continue;

            const auto& instances = manager.GetInstances(volId);
            if (instances.empty())
                continue;

            for (const FoliageInstance& inst : instances)
            {
                if (!inst.visible)
                {
                    ++m_stats.culledInstances;
                    continue;
                }

                // Resolve the species BEFORE emitting the record so that
                // unresolved species (out-of-range volume-local index, or a
                // species name that disappeared from the registry after a
                // registry change) are cleanly skipped instead of entering
                // the batch with a defaulted material id and zero wind
                // strength. Tracked via `stats.unresolvedSpecies` so the
                // renderer's diagnostics still surface the drop.
                if (inst.speciesIndex >= desc->speciesNames.size())
                {
                    ++m_stats.unresolvedSpecies;
                    continue;
                }
                const std::string& speciesName = desc->speciesNames[inst.speciesIndex];
                const FoliageSpecies* species = manager.FindSpecies(speciesName);
                if (!species)
                {
                    ++m_stats.unresolvedSpecies;
                    continue;
                }

                // The material id uploaded to GPUInstanceData must be a
                // registry-wide index, not the volume-local `speciesIndex`.
                // Two volumes that order their `speciesNames` differently
                // would otherwise collide on the same local index and bind
                // the wrong material.
                const uint32_t globalMaterialId = manager.GetSpeciesGlobalIndex(speciesName);
                if (globalMaterialId == UINT32_MAX)
                {
                    ++m_stats.unresolvedSpecies;
                    continue;
                }

                FoliageRenderInstance rec;
                rec.volumeId = volId;
                rec.speciesIndex = inst.speciesIndex;
                rec.globalMaterialId = globalMaterialId;
                rec.distanceToCamera = inst.distanceToCamera;
                rec.windPhase = ComputeWindPhase(inst);
                rec.windStrength = std::clamp(species->windInfluence, 0.0f, 2.0f);
                rec.lod = SelectLOD(inst.distanceToCamera, m_impostorDistance);
                BuildWorldMatrix(inst, rec.worldMatrix);

                if (!species->meshPath.empty())
                {
                    GetOrLoadMesh(species->meshPath);
                }

                if (rec.lod == FoliageRenderLOD::Mesh)
                    ++m_stats.meshDraws;
                else
                    ++m_stats.impostorDraws;

                // Uniqueness key uses the global material id so cross-volume
                // duplicates collapse into a single `uniqueSpecies` count.
                uniqueKeys.insert(static_cast<uint64_t>(globalMaterialId));
                m_renderInstances.push_back(rec);
            }
        }

        m_stats.uniqueSpecies = static_cast<uint32_t>(uniqueKeys.size());

        // Sort the batch by (lod, globalMaterialId) so the GPU render pass
        // can issue one DrawIndexedInstanced per contiguous run. Mesh LOD
        // instances precede impostors — matches the two-sub-pass render
        // flow in RenderFoliagePass (mesh sub-pass then impostor sub-pass).
        std::sort(m_renderInstances.begin(), m_renderInstances.end(),
                  [](const FoliageRenderInstance& a, const FoliageRenderInstance& b)
                  {
                      if (a.lod != b.lod)
                          return static_cast<uint8_t>(a.lod) < static_cast<uint8_t>(b.lod);
                      return a.globalMaterialId < b.globalMaterialId;
                  });

        // Build the draw-run index over the now-sorted batch.
        m_drawOrder.clear();
        if (!m_renderInstances.empty())
        {
            FoliageDrawRun run;
            run.startInstance = 0;
            run.instanceCount = 1;
            run.globalMaterialId = m_renderInstances[0].globalMaterialId;
            run.lod = m_renderInstances[0].lod;

            for (size_t i = 1; i < m_renderInstances.size(); ++i)
            {
                const FoliageRenderInstance& rec = m_renderInstances[i];
                if (rec.lod == run.lod && rec.globalMaterialId == run.globalMaterialId)
                {
                    ++run.instanceCount;
                }
                else
                {
                    m_drawOrder.push_back(run);
                    run.startInstance = static_cast<uint32_t>(i);
                    run.instanceCount = 1;
                    run.globalMaterialId = rec.globalMaterialId;
                    run.lod = rec.lod;
                }
            }
            m_drawOrder.push_back(run);
        }

#ifdef SPARK_PLATFORM_WINDOWS
        // Lazy impostor atlas bake — runs at most once per "species count
        // grew" event. Pulls device + context from the live GraphicsEngine
        // via EngineContext so the test path (no graphics) is unaffected.
        if (auto* ctx = EngineContext::Get())
        {
            if (auto* graphics = ctx->GetGraphics())
            {
                BakeImpostorAtlasIfNeeded(graphics->GetDevice(), graphics->GetContext());
            }
        }
#endif
    }

    // GPU methods (BakeImpostorAtlasIfNeeded, UploadToSceneBuffer,
    // GetOrLoadAlbedoSRV, CreatePassResources, RenderFoliagePass)
    // live in FoliageRendererGPU.cpp — Windows only.

    // (GPU code removed — see FoliageRendererGPU.cpp)

    // ========================================================================
    // Diagnostics
    // ========================================================================

    std::string FoliageRenderer::Console_GetStatus() const
    {
        std::ostringstream os;
        os << "FoliageRenderer: " << m_renderInstances.size() << " instances (" << m_stats.meshDraws << " mesh, "
           << m_stats.impostorDraws << " impostor, " << m_stats.culledInstances << " culled), " << m_meshCache.size()
           << " meshes cached, " << m_stats.meshCacheHits << " hits / " << m_stats.meshCacheMisses << " misses, "
           << m_stats.uniqueSpecies << " unique species";
        return os.str();
    }

} // namespace Spark::Graphics
