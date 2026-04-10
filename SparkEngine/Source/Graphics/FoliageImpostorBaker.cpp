/**
 * @file FoliageImpostorBaker.cpp
 * @brief Implementation of the CPU-side foliage impostor atlas helpers
 *
 * Packing strategy: a simple row-major placement. Every species slot has
 * identical height (`cellSize`) and width (`cellSize * angleSteps`). We
 * advance horizontally until the next slot would exceed `maxAtlasSize`,
 * then wrap to a new row. This is not SAH-optimal but foliage atlases are
 * small (dozens of species, not thousands) and the uniform slot size
 * keeps sampling indices trivial to compute at draw time.
 */

#include "FoliageImpostorBaker.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "AssetPipeline.h"
#include "FoliageSystem.h"
#include "../Utils/SparkConsole.h"
#include <d3dcompiler.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Spark::Graphics
{

    namespace
    {
        constexpr float TWO_PI = 6.283185307179586f;
    }

    uint32_t FoliageImpostorBaker::NextPowerOfTwo(uint32_t v)
    {
        if (v <= 1)
            return 1;
        --v;
        v |= v >> 1;
        v |= v >> 2;
        v |= v >> 4;
        v |= v >> 8;
        v |= v >> 16;
        return v + 1;
    }

    std::vector<ImpostorAtlasSlot> FoliageImpostorBaker::ComputeAtlasLayout(const std::vector<uint32_t>& speciesIndices,
                                                                            uint32_t cellSize, uint32_t angleSteps,
                                                                            uint32_t maxAtlasSize,
                                                                            uint32_t& outAtlasWidth,
                                                                            uint32_t& outAtlasHeight)
    {
        outAtlasWidth = 0;
        outAtlasHeight = 0;

        if (cellSize == 0 || angleSteps == 0 || maxAtlasSize == 0 || speciesIndices.empty())
        {
            return {};
        }

        const uint32_t slotWidth = cellSize * angleSteps;
        if (slotWidth > maxAtlasSize)
        {
            // A single slot cannot fit even into a maxAtlasSize-wide atlas.
            return {};
        }

        std::vector<ImpostorAtlasSlot> slots;
        slots.reserve(speciesIndices.size());

        uint32_t cursorX = 0;
        uint32_t cursorY = 0;
        uint32_t usedWidth = 0;
        uint32_t usedHeight = cellSize;

        for (uint32_t species : speciesIndices)
        {
            if (cursorX + slotWidth > maxAtlasSize)
            {
                // Wrap to a new row.
                cursorX = 0;
                cursorY += cellSize;
            }

            if (cursorY + cellSize > maxAtlasSize)
            {
                // Out of atlas space — drop remaining species rather than
                // silently corrupting coordinates.
                break;
            }

            ImpostorAtlasSlot slot;
            slot.speciesIndex = species;
            slot.atlasX = cursorX;
            slot.atlasY = cursorY;
            slot.cellSize = cellSize;
            slot.angleSteps = angleSteps;
            slots.push_back(slot);

            cursorX += slotWidth;
            usedWidth = std::max(usedWidth, cursorX);
            usedHeight = std::max(usedHeight, cursorY + cellSize);
        }

        // Round used dimensions up to power of two for GPU-friendly sizes.
        outAtlasWidth = std::min(NextPowerOfTwo(usedWidth), maxAtlasSize);
        outAtlasHeight = std::min(NextPowerOfTwo(usedHeight), maxAtlasSize);

        return slots;
    }

    uint32_t FoliageImpostorBaker::SelectAngleSlot(float yawRadians, uint32_t angleSteps)
    {
        if (angleSteps == 0)
            return 0;
        if (angleSteps == 1)
            return 0;

        // Wrap into [0, 2*pi).
        float wrapped = std::fmod(yawRadians, TWO_PI);
        if (wrapped < 0.0f)
            wrapped += TWO_PI;

        // Bin so that yaw=0 lands on the centre of bin 0.
        const float stepAngle = TWO_PI / static_cast<float>(angleSteps);
        const float shifted = wrapped + stepAngle * 0.5f;
        uint32_t bin = static_cast<uint32_t>(shifted / stepAngle);
        if (bin >= angleSteps)
            bin = 0; // wrap edge case when shifted >= 2*pi
        return bin;
    }

    void FoliageImpostorBaker::GetAngleSlotUV(const ImpostorAtlasSlot& slot, uint32_t angleSlotIndex,
                                              uint32_t atlasWidth, uint32_t atlasHeight, float& outMinU, float& outMinV,
                                              float& outMaxU, float& outMaxV)
    {
        outMinU = 0.0f;
        outMinV = 0.0f;
        outMaxU = 0.0f;
        outMaxV = 0.0f;

        if (atlasWidth == 0 || atlasHeight == 0 || slot.cellSize == 0 || slot.angleSteps == 0 ||
            angleSlotIndex >= slot.angleSteps)
        {
            return;
        }

        const uint32_t px = slot.atlasX + angleSlotIndex * slot.cellSize;
        const uint32_t py = slot.atlasY;

        const float invW = 1.0f / static_cast<float>(atlasWidth);
        const float invH = 1.0f / static_cast<float>(atlasHeight);

        outMinU = static_cast<float>(px) * invW;
        outMinV = static_cast<float>(py) * invH;
        outMaxU = static_cast<float>(px + slot.cellSize) * invW;
        outMaxV = static_cast<float>(py + slot.cellSize) * invH;
    }

#ifdef SPARK_PLATFORM_WINDOWS

    // ============================================================================
    // FoliageImpostorAtlas — D3D11 GPU bake implementation
    // ============================================================================

    namespace
    {
        struct ImpostorBakeCB
        {
            DirectX::XMFLOAT4X4 viewProj;
            DirectX::XMFLOAT4 foliageTint;
            DirectX::XMFLOAT3 lightDirection;
            float padding0;
        };
        static_assert(sizeof(ImpostorBakeCB) % 16 == 0, "ImpostorBakeCB must be 16-byte aligned");
    } // namespace

    FoliageImpostorAtlas::~FoliageImpostorAtlas()
    {
        Shutdown();
    }

    bool FoliageImpostorAtlas::Initialize(ID3D11Device* device, uint32_t width, uint32_t height)
    {
        if (!device || width == 0 || height == 0)
            return false;

        // Re-initialising — release previous resources first.
        if (m_initialized)
            Shutdown();

        m_width = width;
        m_height = height;

        // ---- Atlas color texture ------------------------------------
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_atlas)))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FoliageImpostorAtlas: failed to create atlas texture");
            return false;
        }

        if (FAILED(device->CreateRenderTargetView(m_atlas.Get(), nullptr, &m_rtv)))
            return false;

        if (FAILED(device->CreateShaderResourceView(m_atlas.Get(), nullptr, &m_srv)))
            return false;

        // ---- Depth buffer (one per atlas, reused across cells) ------
        D3D11_TEXTURE2D_DESC depthDesc = desc;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&depthDesc, nullptr, &m_depthBuffer)))
            return false;
        if (FAILED(device->CreateDepthStencilView(m_depthBuffer.Get(), nullptr, &m_dsv)))
            return false;

        // ---- Constant buffer ----------------------------------------
        D3D11_BUFFER_DESC cbDesc{};
        cbDesc.ByteWidth = sizeof(ImpostorBakeCB);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&cbDesc, nullptr, &m_constantBuffer)))
            return false;

        // ---- Render states ------------------------------------------
        D3D11_RASTERIZER_DESC rasterDesc{};
        rasterDesc.FillMode = D3D11_FILL_SOLID;
        rasterDesc.CullMode = D3D11_CULL_NONE; // Foliage typically two-sided
        rasterDesc.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rasterDesc, &m_rasterState)))
            return false;

        D3D11_DEPTH_STENCIL_DESC dsDesc{};
        dsDesc.DepthEnable = TRUE;
        dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsDesc.DepthFunc = D3D11_COMPARISON_LESS;
        if (FAILED(device->CreateDepthStencilState(&dsDesc, &m_depthState)))
            return false;

        D3D11_BLEND_DESC blendDesc{};
        blendDesc.RenderTarget[0].BlendEnable = FALSE;
        blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blendDesc, &m_blendState)))
            return false;

        // ---- Compile bake shaders -----------------------------------
        if (!CompileBakeShaders(device))
            return false;

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FoliageImpostorAtlas initialized (%ux%u)", width, height);
        return true;
    }

    bool FoliageImpostorAtlas::CompileBakeShaders(ID3D11Device* device)
    {
        // Source path is relative to the runtime CWD; production code can
        // ship pre-compiled blobs in a future revision.
        const wchar_t* shaderPath = L"Shaders/HLSL/FoliageImpostorBake.hlsl";

        Microsoft::WRL::ComPtr<ID3DBlob> vsBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> psBlob;
        Microsoft::WRL::ComPtr<ID3DBlob> errorBlob;

        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

        HRESULT hr = D3DCompileFromFile(shaderPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0",
                                        flags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr))
        {
            if (errorBlob)
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FoliageImpostorAtlas VS compile error: %s",
                                static_cast<const char*>(errorBlob->GetBufferPointer()));
            return false;
        }

        hr = D3DCompileFromFile(shaderPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", flags, 0,
                                &psBlob, &errorBlob);
        if (FAILED(hr))
        {
            if (errorBlob)
                SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "FoliageImpostorAtlas PS compile error: %s",
                                static_cast<const char*>(errorBlob->GetBufferPointer()));
            return false;
        }

        if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_vs)))
            return false;
        if (FAILED(device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_ps)))
            return false;

        // Input layout matches MeshAssetData::Vertex.
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 44, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT, 0, 68, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"BLENDWEIGHT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 84, D3D11_INPUT_PER_VERTEX_DATA, 0},
        };

        return SUCCEEDED(device->CreateInputLayout(layout, ARRAYSIZE(layout), vsBlob->GetBufferPointer(),
                                                   vsBlob->GetBufferSize(), &m_inputLayout));
    }

    void FoliageImpostorAtlas::Shutdown()
    {
        m_atlas.Reset();
        m_rtv.Reset();
        m_srv.Reset();
        m_depthBuffer.Reset();
        m_dsv.Reset();
        m_vs.Reset();
        m_ps.Reset();
        m_inputLayout.Reset();
        m_constantBuffer.Reset();
        m_rasterState.Reset();
        m_depthState.Reset();
        m_blendState.Reset();
        m_initialized = false;
        m_width = 0;
        m_height = 0;
    }

    bool FoliageImpostorAtlas::BakeSlot(ID3D11DeviceContext* context, const ImpostorAtlasSlot& slot,
                                        const MeshAsset& mesh, const DirectX::XMFLOAT3& bboxMin,
                                        const DirectX::XMFLOAT3& bboxMax, const DirectX::XMFLOAT4& tint)
    {
        if (!m_initialized || !context)
            return false;

        ID3D11Buffer* vb = mesh.GetVertexBuffer();
        ID3D11Buffer* ib = mesh.GetIndexBuffer();
        const uint32_t indexCount = mesh.GetIndexCount();
        if (!vb || !ib || indexCount == 0 || slot.angleSteps == 0 || slot.cellSize == 0)
            return false;

        // Fit an orthographic projection around the bounding box. Use the
        // diagonal of the XZ extent as the ortho width so any yaw rotation
        // still encloses the mesh; height comes from the Y extent.
        const float dx = bboxMax.x - bboxMin.x;
        const float dy = bboxMax.y - bboxMin.y;
        const float dz = bboxMax.z - bboxMin.z;
        const float halfDiagXZ = 0.5f * std::sqrt(dx * dx + dz * dz);
        const float halfHeight = 0.5f * dy;
        const float orthoWidth = std::max(halfDiagXZ * 2.0f, 0.001f);
        const float orthoHeight = std::max(halfHeight * 2.0f, 0.001f);
        const float orthoDepth = std::max(halfDiagXZ * 4.0f, 0.001f);

        const DirectX::XMVECTOR centre = DirectX::XMVectorSet(
            0.5f * (bboxMin.x + bboxMax.x), 0.5f * (bboxMin.y + bboxMax.y), 0.5f * (bboxMin.z + bboxMax.z), 1.0f);
        const DirectX::XMVECTOR up = DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);

        DirectX::XMMATRIX proj = DirectX::XMMatrixOrthographicLH(orthoWidth, orthoHeight, -orthoDepth, orthoDepth);

        // Bind state common to every angle.
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(m_inputLayout.Get());
        const UINT stride = sizeof(MeshAssetData::Vertex);
        const UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);

        context->VSSetShader(m_vs.Get(), nullptr, 0);
        context->PSSetShader(m_ps.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, m_constantBuffer.GetAddressOf());

        context->RSSetState(m_rasterState.Get());
        context->OMSetDepthStencilState(m_depthState.Get(), 0);
        const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        context->OMSetBlendState(m_blendState.Get(), blendFactor, 0xFFFFFFFF);

        ID3D11RenderTargetView* rtvs[] = {m_rtv.Get()};
        context->OMSetRenderTargets(1, rtvs, m_dsv.Get());

        const float twoPi = 6.283185307179586f;

        for (uint32_t angle = 0; angle < slot.angleSteps; ++angle)
        {
            // ---- Per-cell viewport (D3D11 viewport scissors the draw) ----
            D3D11_VIEWPORT vp{};
            vp.TopLeftX = static_cast<float>(slot.atlasX + angle * slot.cellSize);
            vp.TopLeftY = static_cast<float>(slot.atlasY);
            vp.Width = static_cast<float>(slot.cellSize);
            vp.Height = static_cast<float>(slot.cellSize);
            vp.MinDepth = 0.0f;
            vp.MaxDepth = 1.0f;
            context->RSSetViewports(1, &vp);

            // ---- Build view matrix for this yaw ---------------------------
            const float yaw = static_cast<float>(angle) * (twoPi / static_cast<float>(slot.angleSteps));
            const float dist = std::max(orthoDepth, 1.0f);
            DirectX::XMVECTOR eyeOffset = DirectX::XMVectorSet(std::sin(yaw) * dist, 0.0f, std::cos(yaw) * dist, 0.0f);
            DirectX::XMVECTOR eye = DirectX::XMVectorAdd(centre, eyeOffset);
            DirectX::XMMATRIX view = DirectX::XMMatrixLookAtLH(eye, centre, up);
            DirectX::XMMATRIX viewProj = DirectX::XMMatrixMultiply(view, proj);

            // ---- Update constant buffer -----------------------------------
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (FAILED(context->Map(m_constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                continue;
            ImpostorBakeCB cb{};
            DirectX::XMStoreFloat4x4(&cb.viewProj, DirectX::XMMatrixTranspose(viewProj));
            cb.foliageTint = tint;
            cb.lightDirection = DirectX::XMFLOAT3(0.0f, -0.7071f, 0.7071f);
            cb.padding0 = 0.0f;
            std::memcpy(mapped.pData, &cb, sizeof(cb));
            context->Unmap(m_constantBuffer.Get(), 0);

            // Clear only the cell region by drawing into the viewport — we
            // skip the explicit ClearRenderTargetView/ClearDepthStencilView
            // for the colour target so adjacent cells already in the atlas
            // remain untouched. The depth buffer is cleared per cell so
            // each angle starts with a fresh depth state.
            context->ClearDepthStencilView(m_dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

            context->DrawIndexed(indexCount, 0, 0);
        }

        // Detach the render target so subsequent draws don't accidentally
        // keep our SRV bound for write.
        ID3D11RenderTargetView* nullRtvs[] = {nullptr};
        context->OMSetRenderTargets(1, nullRtvs, nullptr);

        return true;
    }

    uint32_t FoliageImpostorAtlas::BakeAllRegisteredSpecies(
        ID3D11Device* device, ID3D11DeviceContext* context, const FoliageManager& manager,
        const std::function<std::shared_ptr<MeshAsset>(const std::string&)>& meshLoader, uint32_t cellSize,
        uint32_t angleSteps, uint32_t maxAtlasSize)
    {
        if (!device || !context || cellSize == 0 || angleSteps == 0)
            return 0;

        const uint32_t speciesCount = manager.GetSpeciesCount();
        if (speciesCount == 0)
            return 0;

        // Build the atlas layout for all currently-registered species.
        std::vector<uint32_t> indices;
        indices.reserve(speciesCount);
        for (uint32_t i = 0; i < speciesCount; ++i)
            indices.push_back(i);

        uint32_t atlasW = 0;
        uint32_t atlasH = 0;
        auto slots =
            FoliageImpostorBaker::ComputeAtlasLayout(indices, cellSize, angleSteps, maxAtlasSize, atlasW, atlasH);
        if (slots.empty() || atlasW == 0 || atlasH == 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                           "FoliageImpostorAtlas: layout failed for %u species (cell=%u steps=%u max=%u)", speciesCount,
                           cellSize, angleSteps, maxAtlasSize);
            return 0;
        }

        // (Re)allocate the atlas at the resulting size. Initialize() is
        // idempotent — it releases existing resources first.
        if (!Initialize(device, atlasW, atlasH))
            return 0;

        const DirectX::XMFLOAT4 defaultTint{0.30f, 0.55f, 0.20f, 1.0f}; // foliage green

        uint32_t baked = 0;
        for (size_t i = 0; i < slots.size(); ++i)
        {
            const FoliageSpecies* species = manager.GetSpeciesByGlobalIndex(static_cast<uint32_t>(i));
            if (!species)
                continue;
            if (species->meshPath.empty())
                continue;

            std::shared_ptr<MeshAsset> mesh = meshLoader ? meshLoader(species->meshPath) : nullptr;
            if (!mesh)
            {
                SPARK_LOG_DEBUG(Spark::LogCategory::Graphics,
                                "FoliageImpostorAtlas: skipping species '%s' — mesh '%s' not loaded",
                                species->name.c_str(), species->meshPath.c_str());
                continue;
            }

            const auto& meshData = mesh->GetMeshData();
            if (BakeSlot(context, slots[i], *mesh, meshData.boundingBoxMin, meshData.boundingBoxMax, defaultTint))
                ++baked;
        }

        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "FoliageImpostorAtlas: baked %u / %u species (atlas %ux%u)", baked,
                       speciesCount, atlasW, atlasH);
        return baked;
    }

#endif // SPARK_PLATFORM_WINDOWS

} // namespace Spark::Graphics
