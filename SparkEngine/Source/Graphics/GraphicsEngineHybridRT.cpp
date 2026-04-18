/**
 * @file GraphicsEngineHybridRT.cpp
 * @brief Platform-agnostic HybridRT post-lighting dispatch.
 *
 * Previously inlined in `GraphicsRenderPipelinesWindows.cpp::RenderDeferred`
 * (≈65 lines of setup + texture wrapping + Execute). Extracted here so
 * both the Windows and Linux/macOS pipelines can call the same helper
 * once the non-Windows pipelines are ready to hook it in.
 *
 * The shared part:
 *   - Assemble camera / light / SSR uniforms from view+proj.
 *   - Call the per-platform `AcquireHybridRTBindings()` for GBuffer + HDR
 *     RHI handles.
 *   - Skip cleanly if `m_hybridRT` is null or bindings aren't ready.
 *   - Dispatch `HybridRTManager::Execute`.
 *
 * Texture wrapping is platform-specific and lives in each platform
 * engine TU (Windows wraps ComPtr<ID3D11Texture2D>; Linux/macOS is a
 * follow-up).
 */

#include "../Core/Platform.h"
#include "GraphicsEngine.h"
#include "HybridRT/HybridRTManager.h"
#include "PostProcessingPipeline.h" // SSRSettings

#include "RHI/RHIResources.h" // IRHITexture definition for unique_ptr dtor

using namespace DirectX;

void GraphicsEngine::DispatchHybridRTPass(Spark::RHI::IRHICommandList* cmd, const XMMATRIX& viewMatrix,
                                          const XMMATRIX& projMatrix)
{
#ifdef SPARK_HYBRID_RT
    if (!m_hybridRT)
        return;

    Spark::Graphics::HybridRTBindings bindings = AcquireHybridRTBindings();
    if (!bindings.IsReady())
        return; // Platform hasn't exposed GBuffer textures yet — skip silently.

    // Derive camera position from the inverse view matrix (translation row).
    XMMATRIX invView = XMMatrixInverse(nullptr, viewMatrix);
    XMFLOAT3 camPos;
    XMStoreFloat3(&camPos, invView.r[3]);

    // Primary directional light direction. Matches the placeholder
    // previously hardcoded at the Windows call site; LightingSystem
    // will feed the real direction in a follow-up.
    XMFLOAT3 lightDir = {0.0f, -1.0f, 0.5f};

    Spark::Graphics::SSRSettings ssrDefaults;

    m_hybridRT->Execute(cmd, viewMatrix, projMatrix, camPos, lightDir, bindings.normals.get(), bindings.depth.get(),
                        bindings.albedo.get(), nullptr, nullptr, bindings.lighting.get(), ssrDefaults);
#else
    (void)cmd;
    (void)viewMatrix;
    (void)projMatrix;
#endif
}
