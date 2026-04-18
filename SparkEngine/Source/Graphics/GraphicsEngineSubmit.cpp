/**
 * @file GraphicsEngineSubmit.cpp
 * @brief Platform-agnostic ECS mesh draw submission for GraphicsEngine.
 *
 * The submission queue (`m_drawList` + `m_drawListSpinlock`) is cross-platform
 * state declared in `GraphicsEngine.h`. The submission helper itself only
 * touches `DirectX::XMMATRIX` math and the spinlock guard — both available
 * on every platform thanks to the `Core/Platform.h` stubs. Keeping it here
 * means `RenderSystem::Update` can call `SubmitMeshForRendering` on Linux
 * and macOS too; previously this was a linker-undefined on non-Windows
 * because the Windows-only TU held the only definition.
 *
 * `ProcessDrawList` lives here in two flavors:
 *
 *   - Windows (in GraphicsEngineWindows.cpp) — D3D11 path with GPU-driven
 *     culling and CPU fallback, both routed through the AssetPipeline's
 *     D3D11 loaders.
 *   - Non-Windows (below) — routes the same MeshDrawCommand queue through
 *     the RHI bridge's `IRHICommandList`. Each command resolves to an
 *     `AssetPipeline::BindMesh / BindMaterial / DrawBoundMesh` trio, same
 *     contract as the Windows CPU draw path. Material sort is preserved so
 *     state changes stay minimal; per-object world matrix upload is
 *     deferred to the shared pipeline state (a constant-buffer upload layer
 *     is the AssetPipeline's job, not this file's).
 *
 * On NullRHI the RHI calls are no-ops but well-formed — the draw-call
 * counter still ticks, which is what headless tooling and tests care
 * about.
 */

#include "../Core/Platform.h"
#include "../Utils/SparkError.h"
#include "../Utils/Validate.h"
#include "GraphicsEngine.h"

#ifndef SPARK_PLATFORM_WINDOWS
#include "AssetPipeline.h"
#include "GraphicsEngineRHI.h"
#include "RHI/RHIResources.h"
#include <algorithm>
#endif

using namespace DirectX;

namespace Spark::Graphics::Detail
{
} // namespace Spark::Graphics::Detail

void GraphicsEngine::SubmitMeshForRendering(std::string_view meshPath, std::string_view materialPath,
                                            const DirectX::XMMATRIX& worldMatrix, bool castShadows)
{
    SPARK_WARN_IF(Spark::LogCategory::Graphics, meshPath.empty(), "SubmitMeshForRendering: empty meshPath");
    SPARK_WARN_IF(Spark::LogCategory::Graphics, materialPath.empty(), "SubmitMeshForRendering: empty materialPath");

    MeshDrawCommand cmd;
    cmd.meshPath = meshPath;
    cmd.materialPath = materialPath;
    XMStoreFloat4x4(&cmd.worldMatrix, worldMatrix);
    cmd.castShadows = castShadows;

    SpinlockGuard guard(m_drawListSpinlock);
    m_drawList.push_back(cmd);
}

#ifndef SPARK_PLATFORM_WINDOWS
// Non-Windows ProcessDrawList — drives the per-frame draw list through the
// RHI bridge's `IRHICommandList`. Mirrors the shape of the Windows CPU draw
// path (sort by material to minimize state changes, bind mesh/material,
// draw, update stats) without the D3D11-specific constant-buffer upload
// (`UpdateBasicConstants`) and `SetBasicShaders` bits — those are
// D3D11-only today. The RHI-bridge equivalents live in the pipeline state
// layer, which the caller (RenderDeferred / RenderForward) is expected to
// have bound before this function runs.
//
// On NullRHI every RHI call is a no-op, so the function reduces to draining
// the queue and counting draws. On Vulkan / OpenGL / Metal with a live
// pipeline state bound, this actually issues indexed draw calls — the RHI
// buffers live on `MeshAsset` now (see `BuildRHIBuffersForMesh` in
// AssetPipelineLinux.cpp).
void GraphicsEngine::ProcessDrawList(const DirectX::XMMATRIX& /*viewMatrix*/, const DirectX::XMMATRIX& /*projMatrix*/)
{
    // Swap under the spinlock so the next frame's submissions don't race
    // with this drain. Preserves capacity on both vectors.
    {
        SpinlockGuard guard(m_drawListSpinlock);
        std::swap(m_drawList, m_processingDrawList);
    }

    std::vector<MeshDrawCommand>& localDrawList = m_processingDrawList;
    if (localDrawList.empty())
        return;

    auto& rhi = Spark::Graphics::Detail::GetRHI();
    if (!rhi.initialized)
    {
        localDrawList.clear();
        return;
    }

    Spark::RHI::IRHICommandList* cmd = rhi.bridge.GetCommandList();
    if (!cmd)
    {
        localDrawList.clear();
        return;
    }

    if (!m_assetPipeline)
    {
        localDrawList.clear();
        return;
    }

    // Sort by material path to minimize rebinds (same heuristic the Windows
    // CPU path uses). Mesh path is the secondary key so the vertex/index
    // buffer binds also cluster.
    std::sort(localDrawList.begin(), localDrawList.end(),
              [](const MeshDrawCommand& a, const MeshDrawCommand& b)
              {
                  if (a.materialPath != b.materialPath)
                      return a.materialPath < b.materialPath;
                  return a.meshPath < b.meshPath;
              });

    cmd->BeginEvent("ProcessDrawList (RHI)");

    std::string_view lastMaterial;
    std::string_view lastMesh;
    for (const auto& draw : localDrawList)
    {
        // Material rebind only when it actually changes. BindMaterial on the
        // non-Windows path records the bound texture; the actual SRV bind
        // lands once TextureAsset grows an RHI texture handle.
        if (draw.materialPath != lastMaterial)
        {
            m_assetPipeline->BindMaterial(draw.materialPath);
            lastMaterial = draw.materialPath;
        }
        // Mesh rebind when the path changes — BindMesh sets VB/IB + topology.
        if (draw.meshPath != lastMesh)
        {
            m_assetPipeline->BindMesh(draw.meshPath);
            lastMesh = draw.meshPath;
        }
        m_assetPipeline->DrawBoundMesh();
        m_statistics.drawCalls++;
    }

    cmd->EndEvent();
    localDrawList.clear();
}
#endif
