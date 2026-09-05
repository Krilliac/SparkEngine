#pragma once
/**
 * @file GraphicsRenderPipelinesShadowPass.h
 * @brief Depth-only shadow-caster draw shared by the D3D11 lighting pass and the render-graph shadow pass.
 *
 * `LightingSystem::RenderShadowMaps` sets the viewport, binds the light's
 * depth-stencil view and clears it, then hands the light's view/projection to
 * a callback. This is the body of that callback: it rasterizes every ECS draw
 * command flagged `MeshDrawCommand::castShadows` into whatever depth target is
 * currently bound, using the basic vertex shader with the pixel shader unbound
 * (the standard D3D11 depth-only setup).
 *
 * It reads the draw list non-destructively (`GraphicsEngine::GetDrawList()`
 * returns a copy) so the geometry pass still draws the same meshes afterwards.
 */
#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS

#include "AssetPipeline.h"
#include "GraphicsEngine.h"

#include <DirectXMath.h>
#include <cstdint>
#include <d3d11.h>
#include <vector>
#include <wrl.h>

namespace Spark::Graphics
{
    /**
     * @brief Rasterize the shadow-casting draw commands into the bound depth target.
     *
     * @param engine     Graphics engine owning the D3D11 context, basic shaders and asset pipeline.
     * @param drawList   This frame's ECS draw commands (pass `engine.GetDrawList()`).
     * @param lightView  Light view matrix supplied by LightingSystem::RenderShadowMaps.
     * @param lightProj  Light projection matrix supplied by LightingSystem::RenderShadowMaps.
     * @return Number of shadow-caster draws actually submitted to the GPU. A caster
     *         whose mesh could not be bound is skipped and not counted, so the value
     *         is a true draw count rather than a loop counter.
     */
    inline uint32_t RenderShadowCasterDepth(GraphicsEngine& engine,
                                            const std::vector<GraphicsEngine::MeshDrawCommand>& drawList,
                                            const DirectX::XMMATRIX& lightView, const DirectX::XMMATRIX& lightProj)
    {
        ID3D11DeviceContext* context = engine.GetContext();
        AssetPipeline* assetPipeline = engine.GetAssetPipeline();
        if (!context || !assetPipeline || drawList.empty())
        {
            return 0;
        }

        // Depth-only: keep the basic vertex shader and input layout (they match
        // the mesh vertex layout the geometry pass uses) and unbind the pixel
        // shader so the pass writes depth and nothing else. The caller's pixel
        // shader is restored before returning.
        Microsoft::WRL::ComPtr<ID3D11PixelShader> previousPixelShader;
        context->PSGetShader(previousPixelShader.GetAddressOf(), nullptr, nullptr);

        engine.SetBasicShaders();
        context->PSSetShader(nullptr, nullptr, 0);

        uint32_t drawCalls = 0;
        for (const auto& command : drawList)
        {
            if (!command.castShadows || command.meshPath.empty())
            {
                continue;
            }

            // Bind first: a mesh that is missing or still loading must not be drawn
            // (DrawBoundMesh would rasterize nothing, or the wrong geometry with this
            // command's transform) and must not inflate the draw count that
            // LightingPass folds into RenderStatistics::drawCalls.
            if (!assetPipeline->BindMesh(command.meshPath))
            {
                continue;
            }

            const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&command.worldMatrix);
            engine.UpdateBasicConstants(world, lightView, lightProj);
            assetPipeline->DrawBoundMesh();
            ++drawCalls;
        }

        context->PSSetShader(previousPixelShader.Get(), nullptr, 0);
        return drawCalls;
    }
} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
