/**
 * @file ModelLoading.cpp
 * @brief Cross-platform model rendering helpers (BindMesh, BindMaterial, DrawBoundMesh)
 *
 * Platform-specific model loading split into:
 *   - ModelLoadingWindows.cpp — D3D11 GPU buffer upload
 *   - ModelLoadingLinux.cpp   — CPU-side mesh storage
 *
 * The Bind* / DrawBoundMesh trio lives here because the non-Windows RHI
 * path needs them too — on Linux / macOS they drive the `IRHICommandList`
 * off the shared bridge singleton (`LinuxRHIState::bridge`) instead of
 * D3D11 `ID3D11DeviceContext`. Having both implementations together keeps
 * the state-tracking contract (`m_boundMeshAsset`) symmetric.
 */
#include "Core/Platform.h"
#include "AssetPipeline.h"
#include "../Utils/Validate.h"

#ifndef SPARK_PLATFORM_WINDOWS
#include "GraphicsEngineRHI.h"
#include "RHI/RHIResources.h"
#endif

// ============================================================================
// RENDERING HELPERS (cross-platform with platform guards)
// ============================================================================

void AssetPipeline::BindMesh(std::string_view meshPath)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    auto it = m_assets.find(meshPath);
    if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
    {
        return;
    }

    auto* meshAsset = dynamic_cast<MeshAsset*>(it->second.get());
    if (!meshAsset)
    {
        return;
    }

    ID3D11Buffer* vertexBuffer = meshAsset->GetVertexBuffer();
    ID3D11Buffer* indexBuffer = meshAsset->GetIndexBuffer();
    if (!vertexBuffer || !indexBuffer)
    {
        return;
    }

    constexpr UINT stride = sizeof(MeshAssetData::Vertex);
    constexpr UINT offset = 0;
    m_context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
    m_context->IASetIndexBuffer(indexBuffer, DXGI_FORMAT_R32_UINT, 0);
    m_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Mirror the non-Windows path and remember the bound mesh — lets
    // DrawBoundMesh draw the right one without re-scanning m_assets.
    m_boundMeshAsset = meshAsset;
#else
    // Non-Windows: drive the `IRHICommandList` off the shared bridge.
    // m_context is a D3D11 stub and unused here; instead we look up the
    // asset, set its RHI vertex/index buffers on the command list, and
    // stash the pointer for DrawBoundMesh. All failure modes (bridge not
    // initialized, asset missing, RHI buffers not built) clear the bound
    // pointer so DrawBoundMesh becomes a no-op rather than drawing a stale
    // mesh.

    m_boundMeshAsset = nullptr;

    auto& rhi = Spark::Graphics::Detail::GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    MeshAsset* meshAsset = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(meshPath);
        if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
            return;
        meshAsset = dynamic_cast<MeshAsset*>(it->second.get());
    }
    if (!meshAsset)
        return;

    Spark::RHI::IRHIBuffer* vb = meshAsset->GetRHIVertexBuffer();
    Spark::RHI::IRHIBuffer* ib = meshAsset->GetRHIIndexBuffer();
    if (!vb || !ib)
    {
        // The CPU-side mesh loaded fine but the RHI upload didn't
        // (e.g. async load happened before the bridge was up). Leave the
        // bound pointer null so DrawBoundMesh skips. Consumers that care
        // can retry via BuildRHIBuffersForMesh.
        return;
    }

    cmd->SetVertexBuffer(vb, /*slot=*/0, /*offset=*/0);
    cmd->SetIndexBuffer(ib, /*offset=*/0);
    cmd->SetPrimitiveTopology(Spark::RHI::RHIPrimitiveTopology::TriangleList);
    m_boundMeshAsset = meshAsset;
#endif
}

void AssetPipeline::BindMaterial(std::string_view materialPath)
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    auto it = m_assets.find(materialPath);
    if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
    {
        return;
    }

    auto* textureAsset = dynamic_cast<TextureAsset*>(it->second.get());
    if (!textureAsset)
    {
        return;
    }

    ID3D11ShaderResourceView* srv = textureAsset->GetSRV();
    if (srv)
    {
        m_context->PSSetShaderResources(0, 1, &srv);
    }
    m_boundTextureAsset = textureAsset;
#else
    // Non-Windows: route through IRHICommandList::SetShaderResource on
    // slot 0 (the pixel-shader "diffuse" slot across every backend). All
    // failure modes (bridge not up, asset missing, RHI texture not built)
    // clear the bound pointer so DrawBoundMesh is free to skip even if
    // subsequent logic was going to depend on the material being live.

    m_boundTextureAsset = nullptr;

    auto& rhi = Spark::Graphics::Detail::GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    TextureAsset* textureAsset = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_assetsMutex);
        auto it = m_assets.find(materialPath);
        if (it == m_assets.end() || !it->second || !it->second->IsLoaded())
            return;
        textureAsset = dynamic_cast<TextureAsset*>(it->second.get());
    }
    if (!textureAsset)
        return;

    Spark::RHI::IRHITexture* rhiTex = textureAsset->GetRHITexture();
    if (rhiTex)
    {
        // Slot 0 is the convention across the engine's shaders for the
        // diffuse / albedo texture. Other channels (normal, roughness, etc.)
        // aren't plumbed through MeshDrawCommand today — that would be a
        // material-asset refactor and is out of scope here.
        cmd->SetShaderResource(Spark::RHI::RHIShaderStage::Pixel, /*slot=*/0, rhiTex);
    }
    // Even when no RHI texture exists (e.g. the bridge came up after this
    // TextureAsset loaded), record the bound pointer — a later retry / hot-
    // reload can upload the texture and re-bind without another lookup.
    m_boundTextureAsset = textureAsset;
#endif
}

void AssetPipeline::DrawBoundMesh()
{
#ifdef SPARK_PLATFORM_WINDOWS
    if (!m_context)
    {
        return;
    }

    // Prefer the explicit bound-mesh pointer; fall back to the legacy
    // "first loaded mesh" scan so existing Windows call sites that never
    // set m_boundMeshAsset still behave.
    if (m_boundMeshAsset && m_boundMeshAsset->GetIndexCount() > 0)
    {
        m_context->DrawIndexed(m_boundMeshAsset->GetIndexCount(), 0, 0);
        return;
    }

    std::lock_guard<std::mutex> lock(m_assetsMutex);

    for (const auto& [path, asset] : m_assets)
    {
        if (!asset || asset->GetType() != AssetType::Mesh || !asset->IsLoaded())
        {
            continue;
        }

        auto* meshAsset = dynamic_cast<MeshAsset*>(asset.get());
        if (meshAsset && meshAsset->GetIndexCount() > 0)
        {
            m_context->DrawIndexed(meshAsset->GetIndexCount(), 0, 0);
            return;
        }
    }
#else
    // Non-Windows: draw the mesh that BindMesh recorded via the RHI
    // command list. Skip cleanly when nothing is bound (or when Bind
    // rejected the mesh because its RHI buffers weren't ready).
    if (!m_boundMeshAsset)
        return;

    auto& rhi = Spark::Graphics::Detail::GetRHI();
    if (!rhi.initialized)
        return;

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
        return;

    const uint32_t indexCount = m_boundMeshAsset->GetIndexCount();
    if (indexCount == 0)
        return;

    cmd->DrawIndexed(indexCount, /*startIndex=*/0, /*baseVertex=*/0);
#endif
}
