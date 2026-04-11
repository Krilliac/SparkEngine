/**
 * @file FoliageRenderer.h
 * @brief Render-side consumer for FoliageManager instances
 * @author Spark Engine Team
 * @date 2026
 *
 * FoliageRenderer is the bridge between the CPU-side `FoliageManager`
 * (which owns species, volumes and scattered instances) and the GPU scene
 * pipeline. Its responsibilities are:
 *
 *   1. Cache loaded mesh assets per species name (via a user-supplied mesh
 *      loader callback so tests can avoid the real AssetPipeline).
 *   2. Translate visible `FoliageInstance`s into
 *      render-batch records grouped by (species, LOD) along with per-instance
 *      wind phase, world matrix and impostor state.
 *   3. Upload the resulting instance data to a `GPUSceneBuffer` on Windows
 *      builds.
 *
 * CPU-only tests cover batch building, wind phase computation and LOD
 * selection; Windows-only code paths (`UploadToSceneBuffer`) are guarded by
 * `SPARK_PLATFORM_WINDOWS` and are no-ops elsewhere.
 *
 * @threadsafety Main thread only (mirrors FoliageManager).
 */

#pragma once

#include "../Core/Platform.h"
#include "FoliageImpostorBaker.h"
#include "FoliageSystem.h"
#include "GPUSceneBuffer.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations to avoid pulling AssetPipeline.h / TextureSystem.h
// here (both are Windows-heavy). The renderer stores assets as opaque
// shared handles and fishes the SRV out inside the Windows-only draw
// path where the full headers are already visible.
class MeshAsset;
class AssetPipeline;
class Texture;
class TextureSystem;

#ifdef SPARK_PLATFORM_WINDOWS
namespace Spark::Graphics
{
    class GPUSceneBuffer;
}
#endif

namespace Spark::Graphics
{

    // ========================================================================
    // FoliageRenderLOD
    // ========================================================================

    /**
     * @brief LOD level chosen for a single foliage instance.
     */
    enum class FoliageRenderLOD : uint8_t
    {
        Mesh = 0,    ///< Full mesh draw using the loaded species asset.
        Impostor = 1 ///< Distant billboard sampled from the impostor atlas.
    };

    /**
     * @brief A single record produced by FoliageRenderer::BuildRenderInstances.
     *
     * Matches the per-instance data that eventually lands in
     * `GPUInstanceData`, plus a few CPU-only fields used for grouping and
     * debugging. Kept header-struct-public so tests can inspect the output
     * without reaching into private state.
     */
    struct FoliageRenderInstance
    {
        uint32_t volumeId = 0;         ///< Source volume.
        uint32_t speciesIndex = 0;     ///< Volume-local species index (0..N in the volume's speciesNames).
        uint32_t globalMaterialId = 0; ///< Global (registry-wide) species index. This is the value
                                       ///< uploaded to `GPUInstanceData::materialId` — using the
                                       ///< volume-local `speciesIndex` there would break material
                                       ///< lookup when two volumes use different species orderings.
        FoliageRenderLOD lod = FoliageRenderLOD::Mesh;
        float distanceToCamera = 0.0f; ///< Last known distance (metres).
        float windPhase = 0.0f;        ///< Per-instance sway phase in radians.
        float windStrength = 0.0f;     ///< Species wind influence, clamped.

        // Full 4x4 world matrix (row-major) ready to copy into GPUInstanceData.
        float worldMatrix[16] = {
            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
        };
    };

    /**
     * @brief A contiguous run of instances sharing `(lod, globalMaterialId)`.
     *
     * Populated by `CollectFromFoliageManager` after sorting the batch so
     * the GPU render pass can issue one `DrawIndexedInstanced` per run
     * without scanning for group boundaries per frame.
     */
    struct FoliageDrawRun
    {
        uint32_t startInstance = 0;    ///< Index into the sorted batch.
        uint32_t instanceCount = 0;    ///< Number of consecutive instances in the run.
        uint32_t globalMaterialId = 0; ///< Species index (for mesh lookup / cell buffer).
        FoliageRenderLOD lod = FoliageRenderLOD::Mesh;
    };

    /**
     * @brief Render statistics reported by the renderer each frame.
     */
    struct FoliageRenderStats
    {
        uint32_t meshDraws = 0;         ///< Mesh-LOD instances in the latest batch.
        uint32_t impostorDraws = 0;     ///< Impostor-LOD instances in the latest batch.
        uint32_t culledInstances = 0;   ///< Instances skipped because they were not visible.
        uint32_t unresolvedSpecies = 0; ///< Instances skipped because their species could not be
                                        ///< resolved (out-of-range local index or species name that
                                        ///< is no longer in the registry after a registry change).
        uint32_t meshCacheHits = 0;     ///< Mesh lookups served from the cache.
        uint32_t meshCacheMisses = 0;   ///< Mesh lookups that triggered a loader callback.
        uint32_t uniqueSpecies = 0;     ///< Number of distinct species in the batch.
    };

    /**
     * @brief Signature of the mesh loader callback.
     *
     * The callback is invoked on a cache miss for a given mesh path. A
     * `nullptr` return is allowed and is treated as "asset not available"
     * — the instance will still be included in the render batch but will
     * be rendered as an impostor instead.
     */
    using FoliageMeshLoader = std::function<std::shared_ptr<MeshAsset>(const std::string&)>;

    // ========================================================================
    // FoliageRenderer
    // ========================================================================

    /**
     * @brief Consumer that pulls from FoliageManager and feeds the GPU.
     *
     * Lifecycle (mirrors FoliageManager):
     *   - Initialize() once at engine start. Pass a mesh loader callback;
     *     production code binds this to `AssetPipeline::LoadMesh`, tests
     *     use a lambda that returns nullptr or a stub handle.
     *   - CollectFromFoliageManager() once per frame after the manager's
     *     Update() has refreshed visibility. Builds the batch and increments
     *     internal stats.
     *   - UploadToSceneBuffer() — Windows only — transfers the batch to a
     *     GPUSceneBuffer for the render graph to draw.
     *   - Shutdown() at engine end.
     */
    class FoliageRenderer
    {
      public:
        FoliageRenderer();
        ~FoliageRenderer();

        FoliageRenderer(const FoliageRenderer&) = delete;
        FoliageRenderer& operator=(const FoliageRenderer&) = delete;

        /**
         * @brief Process-wide singleton used by the engine lifecycle.
         *
         * Tests may continue to create local instances via the public
         * constructor; the singleton is purely for the render loop so it
         * can match the `FoliageManager::GetInstance()` pattern.
         */
        static FoliageRenderer& GetInstance();

        /**
         * @brief Initialize the renderer.
         * @param loader  Callback invoked on mesh cache misses. May be null,
         *                in which case the renderer will attempt to resolve
         *                a default loader from `EngineContext::GetAssetPipeline()`
         *                on the first CollectFromFoliageManager call.
         * @param impostorDistance  Distance beyond which instances render as impostors.
         * @return true on success.
         */
        bool Initialize(FoliageMeshLoader loader, float impostorDistance = 50.0f);

        /**
         * @brief Install an AssetPipeline-backed mesh loader explicitly.
         *
         * Invoked by the engine lifecycle once AssetPipeline is known to be
         * reachable via EngineContext. Can also be called by tests that want
         * to exercise the real AssetPipeline code path with a custom instance.
         * Passing `nullptr` is a no-op.
         *
         * @return true if the loader was installed (pipeline was non-null).
         */
        bool InstallAssetPipelineLoader(::AssetPipeline* pipeline);

        /**
         * @brief True if an explicit (user- or engine-supplied) loader has
         *        been installed. The passthrough loader used when
         *        Initialize() is called with a null loader does NOT count.
         */
        bool HasExplicitLoader() const { return m_hasExplicitLoader; }

        /**
         * @brief Release all cached meshes and statistics.
         */
        void Shutdown();

        /**
         * @brief True after Initialize() and before Shutdown().
         */
        bool IsInitialized() const { return m_initialized; }

        // --------------------------------------------------------------------
        // Per-frame collection
        // --------------------------------------------------------------------

        /**
         * @brief Rebuild the render batch from FoliageManager's current state.
         *
         * Reads every volume's instance list, resolves the per-volume
         * species to a `FoliageSpecies` by name, loads the species mesh
         * through the loader callback (and caches it), and appends one
         * `FoliageRenderInstance` per visible source instance. Culled
         * instances are counted in the stats but not emitted.
         *
         * @param windTime  Global wind clock (seconds). Same value for all instances.
         */
        void CollectFromFoliageManager(float windTime);

        /**
         * @brief Read-only access to the latest batch.
         *
         * After `CollectFromFoliageManager` the batch is sorted by
         * `(lod, globalMaterialId)` so runs of identical species are
         * contiguous and mesh LODs precede impostors — matching
         * `m_drawOrder`.
         */
        const std::vector<FoliageRenderInstance>& GetRenderInstances() const { return m_renderInstances; }

        /**
         * @brief Read-only access to the contiguous draw runs of the
         *        current sorted batch.
         */
        const std::vector<FoliageDrawRun>& GetDrawOrder() const { return m_drawOrder; }

        /**
         * @brief Statistics captured during the last CollectFromFoliageManager call.
         */
        const FoliageRenderStats& GetStats() const { return m_stats; }

        /**
         * @brief Number of meshes currently resident in the cache.
         */
        uint32_t GetMeshCacheSize() const { return static_cast<uint32_t>(m_meshCache.size()); }

        /**
         * @brief Current impostor switch distance.
         */
        float GetImpostorDistance() const { return m_impostorDistance; }

        /**
         * @brief Update the impostor switch distance at runtime.
         */
        void SetImpostorDistance(float distance) { m_impostorDistance = distance; }

        /**
         * @brief Explicitly cache a mesh handle for a given path.
         *
         * Used by tests to inject stub MeshAsset shared_ptrs without running
         * through a loader callback. Overwrites any existing entry for the
         * same path. `nullptr` is accepted to represent "asset unavailable".
         */
        void PrewarmMesh(const std::string& path, std::shared_ptr<MeshAsset> mesh);

        /**
         * @brief True if the mesh cache currently holds an entry for `path`.
         *
         * Returns true even when the cached handle is null (i.e. the loader
         * previously reported the asset as unavailable) so callers can tell
         * "we have tried this path" from "never seen".
         */
        bool HasMeshCached(const std::string& path) const { return m_meshCache.find(path) != m_meshCache.end(); }

        // --------------------------------------------------------------------
        // Pure helpers (testable without GPU / AssetPipeline)
        // --------------------------------------------------------------------

        /**
         * @brief Build a row-major world matrix for a FoliageInstance.
         *
         * Wind deformation is only applied at the vertex shader stage — this
         * helper produces the rest-pose world matrix. Computed as
         * TRS(position, yaw, uniform-scale).
         */
        static void BuildWorldMatrix(const FoliageInstance& instance, float* out16);

        /**
         * @brief Deterministic per-instance wind phase based on position.
         *
         * Uses a simple hash of the quantised world position so every call
         * with the same inputs returns the same value across runs. The phase
         * is returned in radians in [0, 2*pi).
         */
        static float ComputeWindPhase(const FoliageInstance& instance);

        /**
         * @brief Choose an LOD for an instance based on camera distance.
         *
         * Instances closer than `impostorDistance` render as `Mesh`;
         * everything else becomes an `Impostor`.
         */
        static FoliageRenderLOD SelectLOD(float distanceToCamera, float impostorDistance);

        /**
         * @brief Translate a CPU `FoliageRenderInstance` into the GPU POD.
         *
         * Extracted from `UploadToSceneBuffer` so unit tests can hit the
         * encoding path without a D3D11 device. Writes:
         *   - `worldMatrix` and `prevWorldMatrix` from the CPU record
         *   - `normalMatrix` = inverse-transpose(world)
         *   - `materialId`    = registry-wide global species index
         *   - `flags`         = Visible | CastShadow | ReceiveShadow,
         *                       plus `FoliageImpostor` if the CPU record's
         *                       `lod == FoliageRenderLOD::Impostor`
         *   - `lodDistance`   = instance distance to camera
         *   - `padding`       = per-instance wind phase (read by the VS)
         */
        static void BuildGPUInstance(const FoliageRenderInstance& src, GPUInstanceData& out);

        /**
         * @brief Clamp a draw run against the number of instances that were
         *        actually uploaded to the scene buffer this frame.
         *
         * `UploadToSceneBuffer` can stop early when it hits `GPUSceneBuffer`
         * capacity (large static scene + large foliage batch). When that
         * happens the tail of `m_drawOrder` still references instance slots
         * past `uploadedCount` — the VS would sample stale or uninitialized
         * `GPUInstanceData`. This helper returns the clamped
         * `(startInstance, instanceCount)` pair; when the entire run is
         * past the uploaded tail `outInstanceCount` is set to 0 and the
         * caller should skip the draw.
         *
         * Pure function so unit tests can pin the overflow behaviour
         * without a D3D11 device.
         *
         * @param run            Draw run produced by `CollectFromFoliageManager`.
         * @param uploadedCount  Number of instances actually written to
         *                       the scene buffer this frame (return value
         *                       of `UploadToSceneBuffer`).
         * @param outStartInstance  Unchanged — always equals `run.startInstance`.
         * @param outInstanceCount  Clamped instance count (0 when the run
         *                          is entirely past `uploadedCount`).
         */
        static void ClampDrawRunToUploaded(const FoliageDrawRun& run, uint32_t uploadedCount,
                                           uint32_t& outStartInstance, uint32_t& outInstanceCount);

#ifdef SPARK_PLATFORM_WINDOWS
        // --------------------------------------------------------------------
        // Windows-only GPU integration
        // --------------------------------------------------------------------

        /**
         * @brief Copy the latest batch into a GPUSceneBuffer.
         *
         * @param sceneBuffer   The buffer to receive instance data.
         * @param startSlot     First slot to write (allows layering on top
         *                      of a buffer already populated by static
         *                      meshes).
         * @return Number of instances written, or UINT32_MAX on error.
         *
         * @note Wired into `GraphicsEngine::EndFrame()` just before
         *       `GPUSceneBuffer::FlushToGPU()` so the latest CPU batch
         *       reaches the GPU each frame. Phase B note about this
         *       method being unused is no longer accurate.
         */
        uint32_t UploadToSceneBuffer(GPUSceneBuffer& sceneBuffer, uint32_t startSlot) const;

        /**
         * @brief Lazily (re)build the impostor atlas to cover every species
         *        currently registered in `FoliageManager`.
         *
         * Called once per frame from `CollectFromFoliageManager`. The first
         * call after Initialize() always rebuilds; subsequent calls only
         * rebuild if the species count has grown since the previous bake
         * (a game module registered new species after engine startup).
         * Idempotent and cheap when nothing has changed.
         *
         * @param device   D3D11 device used to allocate the atlas.
         * @param context  Immediate context for the bake draw calls.
         * @return Number of species successfully baked on this call (0 if
         *         no rebake was needed or no mesh loader is installed).
         */
        uint32_t BakeImpostorAtlasIfNeeded(ID3D11Device* device, ID3D11DeviceContext* context);

        /// @brief Read-only access to the singleton impostor atlas.
        const FoliageImpostorAtlas& GetImpostorAtlas() const { return m_impostorAtlas; }

        /**
         * @brief Issue the foliage render pass for the current batch.
         *
         * Runs two sub-passes that share a single VS/PS pair:
         *   1. **Mesh sub-pass** — one `DrawIndexedInstanced` per contiguous
         *      `(lod=Mesh, globalMaterialId)` run, binding the species' VB/IB
         *      via the renderer's mesh cache.
         *   2. **Impostor sub-pass** — one `DrawIndexedInstanced` per
         *      `(lod=Impostor, globalMaterialId)` run using a persistent unit
         *      quad; the VS rebuilds the quad as a camera-aligned billboard
         *      and the PS samples `FoliageImpostorAtlas` via the cell SRV.
         *
         * All GPU resources (shaders, input layout, CBs, sampler, quad VB/IB,
         * 1×1 white albedo fallback) are created on the first call via
         * `CreatePassResources`.
         *
         * Skips the impostor sub-pass entirely if no impostor runs exist or
         * the atlas cell SRV is null.
         *
         * @param device       D3D11 device (for lazy resource creation).
         * @param context      Immediate context for draw calls.
         * @param view         Camera view matrix (row-major CPU convention).
         * @param proj         Camera projection matrix.
         * @param cameraPos    Camera world position.
         * @param time         Wall-clock seconds for wind animation.
         * @param sceneBuffer  Scene buffer whose SRV is bound at t0 (produced
         *                     by `UploadToSceneBuffer` the same frame).
         * @param startSlot    Index of the first foliage instance inside the
         *                     scene buffer — used as `StartInstanceLocation`
         *                     so the VS's `SV_InstanceID` reads the correct
         *                     `GPUInstanceData` record.
         * @param uploadedCount  Number of instances actually uploaded to
         *                       the scene buffer this frame (the return
         *                       value of `UploadToSceneBuffer`). Runs that
         *                       reference indices past this value are
         *                       clamped so the VS never samples stale or
         *                       out-of-range `GPUInstanceData`.
         */
        void RenderFoliagePass(ID3D11Device* device, ID3D11DeviceContext* context, const DirectX::XMMATRIX& view,
                               const DirectX::XMMATRIX& proj, const DirectX::XMFLOAT3& cameraPos, float time,
                               GPUSceneBuffer& sceneBuffer, uint32_t startSlot, uint32_t uploadedCount);
#endif

        // --------------------------------------------------------------------
        // Diagnostics
        // --------------------------------------------------------------------

        /**
         * @brief Human-readable one-line status for console display.
         */
        std::string Console_GetStatus() const;

      private:
        std::shared_ptr<MeshAsset> GetOrLoadMesh(const std::string& path);

        /// Try to resolve AssetPipeline via EngineContext and install a
        /// default loader if one has not yet been provided. Called
        /// implicitly on the first CollectFromFoliageManager call so
        /// render-side init does not need to know whether AssetPipeline
        /// was registered before or after FoliageRenderer::Initialize.
        void TryInstallEngineContextLoader();

        FoliageMeshLoader m_loader;
        std::unordered_map<std::string, std::shared_ptr<MeshAsset>> m_meshCache;
        std::vector<FoliageRenderInstance> m_renderInstances;
        std::vector<FoliageDrawRun> m_drawOrder;
        FoliageRenderStats m_stats;
        float m_impostorDistance = 50.0f;
        bool m_initialized = false;
        bool m_hasExplicitLoader = false; ///< True if Initialize/Install supplied a real loader

#ifdef SPARK_PLATFORM_WINDOWS
        FoliageImpostorAtlas m_impostorAtlas; ///< Singleton runtime atlas (Windows-only).
        uint32_t m_lastBakedSpeciesCount = 0; ///< Watermark for lazy rebake.

        // Render pass resources (created lazily by CreatePassResources).
        bool CreatePassResources(ID3D11Device* device);

        /**
         * @brief Resolve a species' albedo texture to an SRV, loading and
         *        caching on the first request.
         *
         * Phase F: replaces the 1x1 white fallback per mesh run. Uses
         * `GraphicsEngine::GetTextureSystem()->LoadTexture` on the first
         * call for a given path, then reuses the cached SRV. An empty
         * path or a failed load returns the 1x1 white SRV so the draw
         * stays safe on missing assets.
         */
        ID3D11ShaderResourceView* GetOrLoadAlbedoSRV(const std::string& path);

        Microsoft::WRL::ComPtr<ID3D11VertexShader> m_passVS;
        Microsoft::WRL::ComPtr<ID3D11PixelShader> m_passPS;
        Microsoft::WRL::ComPtr<ID3D11InputLayout> m_passInputLayout;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbPerFrame;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbWind;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_cbLighting;
        Microsoft::WRL::ComPtr<ID3D11SamplerState> m_sampler;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_impostorQuadVB;
        Microsoft::WRL::ComPtr<ID3D11Buffer> m_impostorQuadIB;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> m_whiteTexture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_whiteSRV;
        bool m_passResourcesReady = false;

        // Per-species albedo cache. Holds the owning shared_ptr alive so
        // the SRV stays valid for as long as the species is referenced.
        std::unordered_map<std::string, std::shared_ptr<::Texture>> m_albedoCache;
#endif
    };

} // namespace Spark::Graphics
