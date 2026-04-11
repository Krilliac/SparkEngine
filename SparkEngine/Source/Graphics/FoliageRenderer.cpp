/**
 * @file FoliageRenderer.cpp
 * @brief Implementation of the foliage render consumer
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
 * without any D3D11 device. The Windows-only `UploadToSceneBuffer` method
 * is guarded and calls into the real `GPUSceneBuffer` only on MSVC/WIN32
 * builds.
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

#ifdef SPARK_PLATFORM_WINDOWS
#include "GPUSceneBuffer.h"
#include "GraphicsEngine.h"
#include <d3dcompiler.h>
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

    // ========================================================================
    // Windows GPU upload
    // ========================================================================

#ifdef SPARK_PLATFORM_WINDOWS
    uint32_t FoliageRenderer::BakeImpostorAtlasIfNeeded(ID3D11Device* device, ID3D11DeviceContext* context)
    {
        if (!m_initialized || !device || !context || !m_loader)
            return 0;

        auto& mgr = FoliageManager::GetInstance();
        if (!mgr.IsInitialized())
            return 0;

        const uint32_t currentCount = mgr.GetSpeciesCount();
        if (currentCount == 0)
            return 0;
        if (currentCount == m_lastBakedSpeciesCount && m_impostorAtlas.IsInitialized())
            return 0; // up to date

        const uint32_t baked =
            m_impostorAtlas.BakeAllRegisteredSpecies(device, context, mgr, m_loader, /*cellSize=*/256,
                                                     /*angleSteps=*/8, /*maxAtlasSize=*/4096);

        // Watermark on the OBSERVED species count even if some species
        // failed to bake (their meshes weren't loaded yet) — the renderer
        // will retry on the next frame when the count next changes.
        m_lastBakedSpeciesCount = currentCount;
        return baked;
    }

    uint32_t FoliageRenderer::UploadToSceneBuffer(GPUSceneBuffer& sceneBuffer, uint32_t startSlot) const
    {
        if (!m_initialized)
            return UINT32_MAX;

        const uint32_t capacity = sceneBuffer.GetCapacity();
        uint32_t written = 0;

        for (const FoliageRenderInstance& inst : m_renderInstances)
        {
            const uint32_t slot = startSlot + written;
            if (slot >= capacity)
                break;

            GPUInstanceData data{};
            BuildGPUInstance(inst, data);
            sceneBuffer.UpdateInstance(slot, data);
            ++written;
        }

        return written;
    }

    // ========================================================================
    // Render pass (Phase E)
    // ========================================================================

    namespace
    {
        struct FoliagePerFrameCB
        {
            DirectX::XMFLOAT4X4 view;
            DirectX::XMFLOAT4X4 projection;
            DirectX::XMFLOAT3 cameraPos;
            float time;
        };

        struct FoliageWindCB
        {
            DirectX::XMFLOAT4 windParams;    // x=strength, y=hFreq, z=vFreq, w=refHeight
            DirectX::XMFLOAT4 windDirection; // xyz=direction, w=gustMul
        };

        struct FoliageLightingCB
        {
            DirectX::XMFLOAT4 sunDirection;  // xyz=dir, w=unused
            DirectX::XMFLOAT4 sunColor;      // rgb=color, a=intensity
            DirectX::XMFLOAT4 ambientColor;  // rgb=fill, a=unused
            DirectX::XMFLOAT4 foliageParams; // x=alphaRef, y=wrap, z=backlit, w=aoStrength
        };

        bool CompileFoliageShader(const wchar_t* path, const char* entryPoint, const char* profile,
                                  Microsoft::WRL::ComPtr<ID3DBlob>& outBlob)
        {
            Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;
            UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
            flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
            HRESULT hr = D3DCompileFromFile(path, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, entryPoint, profile,
                                            flags, 0, &outBlob, &errorBlob);
            if (FAILED(hr))
            {
                if (errorBlob)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FoliageRenderer shader compile error (%s): %s",
                                    entryPoint, static_cast<const char*>(errorBlob->GetBufferPointer()));
                }
                else
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                                    "FoliageRenderer shader compile failed (%s) hr=0x%08X", entryPoint,
                                    static_cast<unsigned int>(hr));
                }
                return false;
            }
            return true;
        }
    } // namespace

    bool FoliageRenderer::CreatePassResources(ID3D11Device* device)
    {
        if (m_passResourcesReady)
            return true;
        if (!device)
            return false;

        // ---- Compile VS + PS ---------------------------------------------
        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        if (!CompileFoliageShader(L"Shaders/HLSL/FoliageVS.hlsl", "main", "vs_5_0", vsBlob))
            return false;
        if (!CompileFoliageShader(L"Shaders/HLSL/FoliagePS.hlsl", "main", "ps_5_0", psBlob))
            return false;

        if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_passVS)))
            return false;
        if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_passPS)))
            return false;

        // ---- Input layout: POSITION/NORMAL/TEXCOORD0 ---------------------
        // Matches the byte offsets of the equivalent fields inside
        // MeshAssetData::Vertex (POSITION at 0, NORMAL at 12, TEXCOORD0 at
        // 36 — bytes 24..35 are TANGENT and intentionally skipped). The
        // impostor unit quad vertex is padded to match the same offsets so
        // a single input layout serves both sub-passes.
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        if (FAILED(device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(),
                                             vsBlob->GetBufferSize(), &m_passInputLayout)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FoliageRenderer: CreateInputLayout failed");
            return false;
        }

        // ---- Constant buffers --------------------------------------------
        auto createCB = [device](UINT byteWidth, Microsoft::WRL::ComPtr<ID3D11Buffer>& out)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (byteWidth + 15u) & ~15u; // round up to 16
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &out));
        };
        if (!createCB(sizeof(FoliagePerFrameCB), m_cbPerFrame))
            return false;
        if (!createCB(sizeof(FoliageWindCB), m_cbWind))
            return false;
        if (!createCB(sizeof(FoliageLightingCB), m_cbLighting))
            return false;

        // ---- Linear-wrap sampler -----------------------------------------
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(device->CreateSamplerState(&sd, &m_sampler)))
            return false;

        // ---- 1x1 white albedo fallback -----------------------------------
        // Mesh instances sample this until a per-species albedo loader
        // lands in a follow-up phase. Impostor instances never touch t1 —
        // they sample the atlas at t2 instead.
        const uint32_t whitePixel = 0xFFFFFFFFu;
        D3D11_TEXTURE2D_DESC td{};
        td.Width = 1;
        td.Height = 1;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA texInit{};
        texInit.pSysMem = &whitePixel;
        texInit.SysMemPitch = sizeof(whitePixel);
        if (FAILED(device->CreateTexture2D(&td, &texInit, &m_whiteTexture)))
            return false;
        if (FAILED(device->CreateShaderResourceView(m_whiteTexture.Get(), nullptr, &m_whiteSRV)))
            return false;

        // ---- Unit quad for impostor billboards ---------------------------
        // VS treats pos.xy as horizontal offsets and pos.y as vertical
        // height for the y-aligned billboard. Layout mirrors the mesh
        // vertex prefix: POSITION@0, NORMAL@12, TANGENT-padding@24 (unused
        // but consumed by stride), TEXCOORD0@36 → 44 bytes per vertex.
        struct QuadVertex
        {
            float px, py, pz;    // 0..11   POSITION
            float nx, ny, nz;    // 12..23  NORMAL
            float tangentPad[3]; // 24..35  unused (matches mesh TANGENT)
            float u, v;          // 36..43  TEXCOORD0
        };
        static_assert(sizeof(QuadVertex) == 44, "QuadVertex must match mesh layout prefix");
        const QuadVertex quadVerts[4] = {
            {-0.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, {0, 0, 0}, 0.0f, 1.0f}, // bottom-left
            {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, -1.0f, {0, 0, 0}, 1.0f, 1.0f},  // bottom-right
            {-0.5f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, {0, 0, 0}, 0.0f, 0.0f}, // top-left
            {0.5f, 1.0f, 0.0f, 0.0f, 0.0f, -1.0f, {0, 0, 0}, 1.0f, 0.0f},  // top-right
        };
        const uint32_t quadIndices[6] = {0, 1, 2, 2, 1, 3};

        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = sizeof(quadVerts);
        vbd.Usage = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vbInit{};
        vbInit.pSysMem = quadVerts;
        if (FAILED(device->CreateBuffer(&vbd, &vbInit, &m_impostorQuadVB)))
            return false;

        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = sizeof(quadIndices);
        ibd.Usage = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA ibInit{};
        ibInit.pSysMem = quadIndices;
        if (FAILED(device->CreateBuffer(&ibd, &ibInit, &m_impostorQuadIB)))
            return false;

        m_passResourcesReady = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FoliageRenderer: pass resources created");
        return true;
    }

    void FoliageRenderer::RenderFoliagePass(ID3D11Device* device, ID3D11DeviceContext* context,
                                            const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
                                            const DirectX::XMFLOAT3& cameraPos, float time, GPUSceneBuffer& sceneBuffer,
                                            uint32_t startSlot)
    {
        if (!m_initialized || !device || !context)
            return;
        if (m_drawOrder.empty() || !sceneBuffer.IsInitialized())
            return;
        if (!CreatePassResources(device))
            return;

        // ---- Update constant buffers -------------------------------------
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(m_cbPerFrame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                FoliagePerFrameCB cb{};
                // Transpose to match HLSL default column-major packing; the
                // VS uses `mul(v, View)` which expects transposed input.
                DirectX::XMStoreFloat4x4(&cb.view, DirectX::XMMatrixTranspose(view));
                DirectX::XMStoreFloat4x4(&cb.projection, DirectX::XMMatrixTranspose(proj));
                cb.cameraPos = cameraPos;
                cb.time = time;
                std::memcpy(mapped.pData, &cb, sizeof(cb));
                context->Unmap(m_cbPerFrame.Get(), 0);
            }
        }
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(m_cbWind.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                FoliageWindCB cb{};
                cb.windParams = DirectX::XMFLOAT4(0.5f, 0.25f, 0.1f, 2.0f);
                cb.windDirection = DirectX::XMFLOAT4(0.7071f, 0.0f, 0.7071f, 1.0f);
                std::memcpy(mapped.pData, &cb, sizeof(cb));
                context->Unmap(m_cbWind.Get(), 0);
            }
        }
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(m_cbLighting.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                FoliageLightingCB cb{};
                cb.sunDirection = DirectX::XMFLOAT4(0.0f, -0.7071f, 0.7071f, 0.0f);
                cb.sunColor = DirectX::XMFLOAT4(1.0f, 0.95f, 0.8f, 1.2f);
                cb.ambientColor = DirectX::XMFLOAT4(0.25f, 0.3f, 0.35f, 0.0f);
                cb.foliageParams = DirectX::XMFLOAT4(0.3f, 0.4f, 0.3f, 0.5f);
                std::memcpy(mapped.pData, &cb, sizeof(cb));
                context->Unmap(m_cbLighting.Get(), 0);
            }
        }

        // ---- Bind pipeline state -----------------------------------------
        context->VSSetShader(m_passVS.Get(), nullptr, 0);
        context->PSSetShader(m_passPS.Get(), nullptr, 0);
        context->IASetInputLayout(m_passInputLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        ID3D11Buffer* vsCBs[] = {m_cbPerFrame.Get(), nullptr, m_cbWind.Get()};
        context->VSSetConstantBuffers(0, 3, vsCBs);
        ID3D11Buffer* psCBs[] = {m_cbPerFrame.Get(), nullptr, nullptr, m_cbLighting.Get()};
        context->PSSetConstantBuffers(0, 4, psCBs);

        ID3D11SamplerState* samplers[] = {m_sampler.Get()};
        context->PSSetSamplers(0, 1, samplers);

        // Bind GPU scene buffer SRV at t0 for VS.
        sceneBuffer.BindVS(context, 0);

        // Bind albedo fallback at t1 (mesh path) and impostor atlas SRVs at
        // t2/t3 (impostor path). Both sub-passes share the bind state.
        ID3D11ShaderResourceView* atlasSRV = m_impostorAtlas.GetSRV();
        ID3D11ShaderResourceView* cellSRV = m_impostorAtlas.GetCellSRV();
        ID3D11ShaderResourceView* psSRVs[] = {m_whiteSRV.Get(), atlasSRV, cellSRV};
        context->PSSetShaderResources(1, 3, psSRVs);
        // The VS also needs the cell SRV at t3 for the angle-bin lookup.
        ID3D11ShaderResourceView* vsCellSRVs[] = {cellSRV};
        context->VSSetShaderResources(3, 1, vsCellSRVs);

        auto& manager = FoliageManager::GetInstance();

        // ---- Mesh sub-pass: one DrawIndexedInstanced per species run -----
        for (const FoliageDrawRun& run : m_drawOrder)
        {
            if (run.lod != FoliageRenderLOD::Mesh)
                continue;

            const FoliageSpecies* species = manager.GetSpeciesByGlobalIndex(run.globalMaterialId);
            if (!species || species->meshPath.empty())
                continue;

            auto it = m_meshCache.find(species->meshPath);
            if (it == m_meshCache.end() || !it->second)
                continue;

            MeshAsset* mesh = it->second.get();
            ID3D11Buffer* vb = mesh->GetVertexBuffer();
            ID3D11Buffer* ib = mesh->GetIndexBuffer();
            const uint32_t indexCount = mesh->GetIndexCount();
            if (!vb || !ib || indexCount == 0)
                continue;

            // MeshAssetData::Vertex stride — first three input-layout
            // elements span 32 bytes, but the full vertex is larger
            // (POSITION+NORMAL+TANGENT+TEXCOORD0+TEXCOORD1+COLOR+indices+
            // weights). The engine's mesh stride is 100 bytes; we step by
            // that so SV_VertexID advances correctly through the VB.
            const UINT stride = 100u;
            const UINT offset = 0u;
            context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);

            context->DrawIndexedInstanced(indexCount, run.instanceCount, 0, 0, startSlot + run.startInstance);
        }

        // ---- Impostor sub-pass: one DrawIndexedInstanced per species run
        // Skip entirely if the atlas cell SRV never got built.
        if (cellSRV != nullptr && atlasSRV != nullptr)
        {
            const UINT quadStride = 44u;
            const UINT quadOffset = 0u;
            ID3D11Buffer* quadVB = m_impostorQuadVB.Get();
            context->IASetVertexBuffers(0, 1, &quadVB, &quadStride, &quadOffset);
            context->IASetIndexBuffer(m_impostorQuadIB.Get(), DXGI_FORMAT_R32_UINT, 0);

            for (const FoliageDrawRun& run : m_drawOrder)
            {
                if (run.lod != FoliageRenderLOD::Impostor)
                    continue;
                context->DrawIndexedInstanced(6u, run.instanceCount, 0, 0, startSlot + run.startInstance);
            }
        }

        // ---- Unbind SRVs so the scene buffer / atlas can be written next
        // frame without a debug-layer "resource bound as SRV and RTV" error
        ID3D11ShaderResourceView* nullSRVs[4] = {};
        context->VSSetShaderResources(0, 4, nullSRVs);
        context->PSSetShaderResources(1, 3, nullSRVs);
    }
#endif // SPARK_PLATFORM_WINDOWS

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
