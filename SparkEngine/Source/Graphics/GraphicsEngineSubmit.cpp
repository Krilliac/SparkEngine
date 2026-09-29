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
 *   - Non-Windows (below) — the forward draw-list pass on the RHI bridge's
 *     `IRHICommandList`. It binds the basic_vs/basic_ps pipeline built by
 *     InitializeBasicShaders, uploads the per-frame constants once, and for
 *     every command uploads its per-object constants and runs the
 *     `AssetPipeline::BindMesh / BindMaterial / DrawBoundMesh` trio, the same
 *     contract as the Windows CPU draw path. Material sort is preserved so
 *     state changes stay minimal.
 *
 * Draws are counted by the RHI backend as they are recorded (folded into
 * RenderStatistics::drawCalls by EndFrame), never by this file. Without the
 * pipeline the pass records nothing: the draws are counted as rejected and
 * logged instead of being recorded unbound.
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
#include <cmath>
#endif

using namespace DirectX;

void GraphicsEngine::SubmitMeshForRendering(std::string_view meshPath, std::string_view materialPath,
                                            const DirectX::XMMATRIX& worldMatrix, bool castShadows)
{
    SPARK_WARN_IF(Spark::LogCategory::Graphics, meshPath.empty(), "SubmitMeshForRendering: empty meshPath");
    // An empty material path is a valid default-material selection. Starter
    // scenes deliberately leave this empty for primitive and OBJ geometry;
    // warning here turned every visible default-material mesh into a per-frame
    // diagnostic storm while the renderer correctly used its default texture.

    MeshDrawCommand cmd;
    cmd.meshPath = meshPath;
    cmd.materialPath = materialPath;
    XMStoreFloat4x4(&cmd.worldMatrix, worldMatrix);
    cmd.castShadows = castShadows;

    SpinlockGuard guard(m_drawListSpinlock);
    m_drawList.push_back(cmd);
}

#ifndef SPARK_PLATFORM_WINDOWS
namespace
{
    using Spark::Graphics::Detail::BasicFrameConstants;
    using Spark::Graphics::Detail::BasicObjectConstants;

    /// Projection as the active backend's clip space needs it. The engine builds D3D-style
    /// matrices (clip Y up, depth 0..1). OpenGL shares the Y direction; Vulkan's clip Y points
    /// down, so its image would be upside down without the flip.
    XMMATRIX BackendProjection(const XMMATRIX& projMatrix, Spark::RHI::GraphicsBackend backend)
    {
        if (backend == Spark::RHI::GraphicsBackend::Vulkan)
            return XMMatrixMultiply(projMatrix, XMMatrixScaling(1.0f, -1.0f, 1.0f));
        return projMatrix;
    }

    /// Camera part of PerFrameConstants. DirectXMath matrices are stored untransposed: GLSL
    /// reads the row-major storage as the transposed matrix, which is what `M * v` needs.
    void StoreFrameCamera(BasicFrameConstants& frame, const XMMATRIX& view, const XMMATRIX& projection)
    {
        const XMMATRIX invView = XMMatrixInverse(nullptr, view);
        XMStoreFloat4x4(&frame.view, view);
        XMStoreFloat4x4(&frame.projection, projection);
        XMStoreFloat4x4(&frame.viewProjection, XMMatrixMultiply(view, projection));
        XMStoreFloat3(&frame.cameraPosition, invView.r[3]);
        XMStoreFloat3(&frame.cameraDirection, XMVector3Normalize(invView.r[2]));
    }

    /// PerObjectConstants for one draw, with the basic material defaults of the D3D11 path
    /// (UpdateBasicConstants): white object colour, metallic 0, roughness 0.5, opaque.
    void StoreObjectConstants(BasicObjectConstants& object, const XMFLOAT4X4& worldMatrix, const XMMATRIX& viewProj)
    {
        const XMMATRIX world = XMLoadFloat4x4(&worldMatrix);
        object.world = worldMatrix;
        object.previousWorld = worldMatrix;
        XMStoreFloat4x4(&object.worldViewProjection, XMMatrixMultiply(world, viewProj));
        // Normals transform by the inverse transpose; stored transposed once more for GLSL.
        XMStoreFloat4x4(&object.worldInverseTranspose, XMMatrixTranspose(XMMatrixInverse(nullptr, world)));
        object.objectPosition = XMFLOAT3(worldMatrix._41, worldMatrix._42, worldMatrix._43);
        object.objectScale = 1.0f;
        object.objectColor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
        object.materialProperties = XMFLOAT4(0.0f, 0.5f, 0.0f, 1.0f);
        object.uvTiling = XMFLOAT4(1.0f, 1.0f, 0.0f, 0.0f);
    }

    /// Grows the per-object constant pool to @p drawCount buffers (one per draw, see
    /// BasicForwardPass) and returns how many draws have a buffer. The pool only grows when a
    /// frame has more draws than any before it, so steady-state frames allocate nothing.
    size_t ReserveObjectConstants(Spark::Graphics::Detail::LinuxRHIState& rhi, size_t drawCount)
    {
        auto& pool = rhi.basicForward.objectConstants;
        while (pool.size() < drawCount)
        {
            auto buffer = rhi.bridge.CreateConstantBuffer(sizeof(BasicObjectConstants));
            if (!buffer)
                break;
            pool.push_back(std::move(buffer));
        }
        return std::min(pool.size(), drawCount);
    }
} // namespace

// Non-Windows ProcessDrawList — the forward draw-list pass. Mirrors the
// Windows CPU draw path: sort by material to minimize state changes, then per
// command upload the object constants, bind mesh and material, and draw. The
// RHI buffers live on `MeshAsset` (see `BuildRHIBuffersForMesh` in
// AssetPipelineLinuxStreaming.cpp).
void GraphicsEngine::ProcessDrawList(const DirectX::XMMATRIX& viewMatrix, const DirectX::XMMATRIX& projMatrix)
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

    Spark::RHI::IRHIDevice* device = rhi.bridge.GetDevice();
    auto& pass = rhi.basicForward;
    if (!m_assetPipeline || !device || !pass.pipeline || !rhi.defaultTexture)
    {
        // Fail closed: a draw recorded without the pass pipeline is not a draw.
        pass.rejectedDraws += localDrawList.size();
        SPARK_LOG_ONCE(Spark::LogLevel::Error, Spark::LogCategory::Graphics,
                       "ProcessDrawList: the basic forward pipeline is unavailable; %zu draw(s) rejected this frame "
                       "instead of being recorded unbound",
                       localDrawList.size());
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

    const size_t drawable = ReserveObjectConstants(rhi, localDrawList.size());
    if (drawable < localDrawList.size())
    {
        const size_t rejected = localDrawList.size() - drawable;
        pass.rejectedDraws += rejected;
        SPARK_LOG_ONCE(Spark::LogLevel::Error, Spark::LogCategory::Graphics,
                       "ProcessDrawList: per-object constant buffer allocation failed; %zu draw(s) rejected", rejected);
        localDrawList.resize(drawable);
    }

    const XMMATRIX projection = BackendProjection(projMatrix, rhi.bridge.GetActiveBackend());
    const XMMATRIX viewProj = XMMatrixMultiply(viewMatrix, projection);

    BasicFrameConstants frame{};
    StoreFrameCamera(frame, viewMatrix, projection);
    frame.deltaTime = m_statistics.frameTime / 1000.0f;
    frame.screenResolution = XMFLOAT2(static_cast<float>(m_width), static_cast<float>(m_height));
    frame.invScreenResolution = XMFLOAT2(m_width > 0 ? 1.0f / static_cast<float>(m_width) : 0.0f,
                                         m_height > 0 ? 1.0f / static_cast<float>(m_height) : 0.0f);
    // The sun and ambient of the D3D11 basic path, including a module's SetEnvironmentLighting.
    frame.lightDirection = m_envLightDir;
    frame.lightIntensity = m_envLightIntensity;
    frame.lightColor = m_envLightColor;
    frame.ambientIntensity = m_envAmbientIntensity;
    frame.ambientColor = m_envAmbientColor;
    device->UpdateBuffer(pass.frameConstants.get(), &frame, sizeof(frame));

    cmd->BeginEvent("ProcessDrawList (RHI)");
    cmd->SetPipelineState(pass.pipeline.get());
    cmd->SetConstantBuffer(Spark::RHI::RHIShaderStage::Vertex, 0, pass.frameConstants.get());
    cmd->SetConstantBuffer(Spark::RHI::RHIShaderStage::Pixel, 2, pass.materialConstants.get());
    // BasicPS samples five textures. BindMaterial replaces slot 0 (albedo); the basic material
    // has no normal, metallic-roughness, occlusion or emissive map, so those keep the white
    // default that the material constants neutralize.
    constexpr uint32_t kBasicTextureSlots = 5;
    for (uint32_t slot = 0; slot < kBasicTextureSlots; ++slot)
    {
        cmd->SetShaderResource(Spark::RHI::RHIShaderStage::Pixel, slot, rhi.defaultTexture.get());
        cmd->SetSampler(Spark::RHI::RHIShaderStage::Pixel, slot, pass.sampler.get());
    }

    std::string_view lastMaterial;
    std::string_view lastMesh;
    for (size_t drawIndex = 0; drawIndex < localDrawList.size(); ++drawIndex)
    {
        const MeshDrawCommand& draw = localDrawList[drawIndex];
        // Material rebind only when it actually changes. An empty path keeps the
        // default texture bound above.
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

        Spark::RHI::IRHIBuffer* objectBuffer = pass.objectConstants[drawIndex].get();
        Spark::Graphics::Detail::BasicObjectConstants object{};
        StoreObjectConstants(object, draw.worldMatrix, viewProj);
        device->UpdateBuffer(objectBuffer, &object, sizeof(object));
        cmd->SetConstantBuffer(Spark::RHI::RHIShaderStage::Vertex, 1, objectBuffer);
        m_assetPipeline->DrawBoundMesh();
    }

    cmd->EndEvent();
    localDrawList.clear();
}
#endif
