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
 * `ProcessDrawList` stays Windows-only for now — that path touches D3D11
 * constant buffers, the GPU-driven renderer, and the asset pipeline's
 * D3D11 mesh/material loaders. Linux/macOS consumers should drive meshes
 * through the Metal RT scene feeder or the RHI bridge instead.
 */

#include "../Core/Platform.h"
#include "../Utils/SparkError.h"
#include "../Utils/Validate.h"
#include "GraphicsEngine.h"

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
// Non-Windows ProcessDrawList — drains the per-frame draw list so it
// doesn't grow unbounded on Linux/macOS, and logs a one-shot warning.
//
// The real D3D11 consumer in `GraphicsEngineWindows.cpp` resolves each
// command through the AssetPipeline, binds mesh/material constant
// buffers, and issues draw calls. Doing the same through the RHI bridge
// requires a bridge-level mesh/material binding API that doesn't exist
// yet — until it does, Linux/macOS rendering goes through the legacy
// `std::vector<GameObject*>` path inside `RenderDeferred`/`RenderForward`.
//
// Clearing the queue here keeps the two paths from stepping on each
// other: `RenderSystem::Update` submits per-entity, this function
// drains, the next frame starts fresh. On macOS the RT scene feeder
// bypasses this list entirely — it walks the ECS directly into
// `HybridRTManager`.
void GraphicsEngine::ProcessDrawList(const DirectX::XMMATRIX& /*viewMatrix*/, const DirectX::XMMATRIX& /*projMatrix*/)
{
    static std::atomic_flag s_warned = ATOMIC_FLAG_INIT;
    std::size_t drained = 0;
    {
        SpinlockGuard guard(m_drawListSpinlock);
        drained = m_drawList.size();
        m_drawList.clear();
    }

    if (drained > 0 && !s_warned.test_and_set(std::memory_order_acq_rel))
    {
        SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                       "ProcessDrawList (non-Windows): drained %zu queued commands without rendering. "
                       "Non-Windows pipelines render through RenderDeferred/RenderForward; this path "
                       "is reserved for the future RHI-bridge consumer.",
                       drained);
    }
}
#endif
