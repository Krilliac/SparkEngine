/**
 * @file FoliageRendererWindows.cpp
 * @brief GPU-side foliage rendering — D3D11 upload, shader compilation, draw calls
 *
 * Split from FoliageRenderer.cpp (Phase Theme 3 refactor). Contains all
 * Windows-only GPU methods: impostor atlas baking, scene buffer upload,
 * albedo texture loading, pass resource creation, and the two-sub-pass
 * foliage render (mesh + impostor).
 *
 * The CPU-portable half (collection, batching, LOD selection, wind phase)
 * remains in FoliageRenderer.cpp and runs on all platforms.
 */

#include "FoliageRenderer.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "../Core/EngineContext.h"
#include "../Utils/Validate.h"
#include "AssetPipeline.h"
#include "GPUSceneBuffer.h"
#include "GraphicsEngine.h"
#include "TextureSystem.h"

#include <cstring>
#include <d3dcompiler.h>

namespace Spark::Graphics
{

    // ========================================================================
    // Impostor atlas baking
    // ========================================================================

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

    // ========================================================================
    // Scene buffer upload
    // ========================================================================

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

    // ========================================================================
    // Albedo texture loading
    // ========================================================================

    ID3D11ShaderResourceView* FoliageRenderer::GetOrLoadAlbedoSRV(const std::string& path)
    {
        // Empty path → always the 1x1 white fallback. Callers reach here
        // when a species has no explicit albedoTexturePath set.
        if (path.empty())
            return m_whiteSRV.Get();

        auto it = m_albedoCache.find(path);
        if (it != m_albedoCache.end())
        {
            if (it->second)
                return it->second->GetSRV();
            return m_whiteSRV.Get();
        }

        // Miss — resolve TextureSystem via GraphicsEngine. When the
        // engine is not up (unit tests, early init), keep the fall-back
        // so the draw path stays crash-safe.
        TextureSystem* texSys = nullptr;
        if (auto* ctx = EngineContext::Get())
        {
            if (auto* graphics = ctx->GetGraphics())
                texSys = graphics->GetTextureSystem();
        }
        if (!texSys)
        {
            m_albedoCache.emplace(path, nullptr);
            return m_whiteSRV.Get();
        }

        std::shared_ptr<::Texture> tex = texSys->LoadTexture(path);
        if (!tex || !tex->GetSRV())
        {
            SPARK_LOG_DEBUG(Spark::LogCategory::Graphics,
                            "FoliageRenderer: albedo load failed for '%s' — using white fallback", path.c_str());
            m_albedoCache.emplace(path, nullptr);
            return m_whiteSRV.Get();
        }

        ID3D11ShaderResourceView* srv = tex->GetSRV();
        m_albedoCache.emplace(path, std::move(tex));
        return srv;
    }

    // ========================================================================
    // Pass resource creation
    // ========================================================================

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

    // ========================================================================
    // Foliage render pass — mesh + impostor sub-passes
    // ========================================================================

    void FoliageRenderer::RenderFoliagePass(ID3D11Device* device, ID3D11DeviceContext* context,
                                            const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj,
                                            const DirectX::XMFLOAT3& cameraPos, float time, GPUSceneBuffer& sceneBuffer,
                                            uint32_t startSlot, uint32_t uploadedCount)
    {
        if (!m_initialized || !device || !context)
            return;
        if (m_drawOrder.empty() || !sceneBuffer.IsInitialized())
            return;
        if (uploadedCount == 0)
            return;
        if (!CreatePassResources(device))
            return;

        // ---- Update constant buffers -------------------------------------
        {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(context->Map(m_cbPerFrame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                FoliagePerFrameCB cb{};
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

        // Bind impostor atlas SRVs at t2/t3 once.
        ID3D11ShaderResourceView* atlasSRV = m_impostorAtlas.GetSRV();
        ID3D11ShaderResourceView* cellSRV = m_impostorAtlas.GetCellSRV();
        ID3D11ShaderResourceView* atlasAndCell[] = {atlasSRV, cellSRV};
        context->PSSetShaderResources(2, 2, atlasAndCell);
        ID3D11ShaderResourceView* vsCellSRVs[] = {cellSRV};
        context->VSSetShaderResources(3, 1, vsCellSRVs);

        auto& manager = FoliageManager::GetInstance();

        // ---- Mesh sub-pass: one DrawIndexedInstanced per species run -----
        for (const FoliageDrawRun& run : m_drawOrder)
        {
            if (run.lod != FoliageRenderLOD::Mesh)
                continue;

            uint32_t clampedStart = 0;
            uint32_t clampedCount = 0;
            ClampDrawRunToUploaded(run, uploadedCount, clampedStart, clampedCount);
            if (clampedCount == 0)
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

            ID3D11ShaderResourceView* albedoSRV = GetOrLoadAlbedoSRV(species->albedoTexturePath);
            context->PSSetShaderResources(1, 1, &albedoSRV);

            const UINT stride = 100u;
            const UINT offset = 0u;
            context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
            context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);

            context->DrawIndexedInstanced(indexCount, clampedCount, 0, 0, startSlot + clampedStart);
        }

        // Impostor sub-pass
        ID3D11ShaderResourceView* whiteAlbedo = m_whiteSRV.Get();
        context->PSSetShaderResources(1, 1, &whiteAlbedo);

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

                uint32_t clampedStart = 0;
                uint32_t clampedCount = 0;
                ClampDrawRunToUploaded(run, uploadedCount, clampedStart, clampedCount);
                if (clampedCount == 0)
                    continue;

                context->DrawIndexedInstanced(6u, clampedCount, 0, 0, startSlot + clampedStart);
            }
        }

        // ---- Unbind SRVs
        ID3D11ShaderResourceView* nullSRVs[4] = {};
        context->VSSetShaderResources(0, 4, nullSRVs);
        context->PSSetShaderResources(1, 3, nullSRVs);
    }

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
