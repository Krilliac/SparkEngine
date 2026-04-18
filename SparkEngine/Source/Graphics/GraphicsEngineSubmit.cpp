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
