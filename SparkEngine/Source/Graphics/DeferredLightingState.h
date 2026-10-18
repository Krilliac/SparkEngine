/**
 * @file DeferredLightingState.h
 * @brief Render-thread scope restoring the basic renderer D3D11 bindings.
 * Retains COM references for one resolve without allocating GPU resources;
 * the borrowed immediate context must outlive this scope.
 */
#pragma once
#include "../Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>

namespace Spark::Graphics
{
    // Render-thread stack scope for the basic renderer (which uses no dynamic
    // shader linkage). No GPU allocations; retain and restore every changed slot.
    struct DeferredLightingState
    {
        template <typename T> using Ptr = Microsoft::WRL::ComPtr<T>;
        ID3D11DeviceContext* context;
        Ptr<ID3D11VertexShader> vs;
        Ptr<ID3D11PixelShader> ps;
        Ptr<ID3D11GeometryShader> gs;
        Ptr<ID3D11HullShader> hs;
        Ptr<ID3D11DomainShader> ds;
        Ptr<ID3D11InputLayout> layout;
        Ptr<ID3D11RasterizerState> raster;
        Ptr<ID3D11DepthStencilState> depth;
        Ptr<ID3D11BlendState> blend;
        Ptr<ID3D11Buffer> constants;
        Ptr<ID3D11ShaderResourceView> resources[4];
        Ptr<ID3D11RenderTargetView> targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT];
        Ptr<ID3D11DepthStencilView> depthTarget;
        D3D11_PRIMITIVE_TOPOLOGY topology{};
        D3D11_VIEWPORT
        viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        UINT stencilReference = 0;
        UINT sampleMask = 0;
        FLOAT blendFactor[4]{};

        explicit DeferredLightingState(ID3D11DeviceContext* value) : context(value)
        {
            context->VSGetShader(vs.GetAddressOf(), nullptr, nullptr);
            context->PSGetShader(ps.GetAddressOf(), nullptr, nullptr);
            context->GSGetShader(gs.GetAddressOf(), nullptr, nullptr);
            context->HSGetShader(hs.GetAddressOf(), nullptr, nullptr);
            context->DSGetShader(ds.GetAddressOf(), nullptr, nullptr);
            context->IAGetInputLayout(layout.GetAddressOf());
            context->IAGetPrimitiveTopology(&topology);
            context->RSGetState(raster.GetAddressOf());
            context->RSGetViewports(&viewportCount, viewports);
            context->OMGetDepthStencilState(depth.GetAddressOf(), &stencilReference);
            context->OMGetBlendState(blend.GetAddressOf(), blendFactor, &sampleMask);
            context->PSGetConstantBuffers(0, 1, constants.GetAddressOf());
            for (UINT i = 0; i < 4; ++i)
                context->PSGetShaderResources(i, 1, resources[i].GetAddressOf());
            ID3D11RenderTargetView* rawTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawTargets, depthTarget.GetAddressOf());
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                targets[i].Attach(rawTargets[i]);
        }

        ~DeferredLightingState()
        {
            ID3D11ShaderResourceView* empty[4]{};
            context->PSSetShaderResources(0, 4, empty);
            ID3D11RenderTargetView* rawTargets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
            for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i)
                rawTargets[i] = targets[i].Get();
            context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rawTargets, depthTarget.Get());
            for (UINT i = 0; i < 4; ++i)
                context->PSSetShaderResources(i, 1, resources[i].GetAddressOf());
            context->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(ps.Get(), nullptr, 0);
            context->GSSetShader(gs.Get(), nullptr, 0);
            context->HSSetShader(hs.Get(), nullptr, 0);
            context->DSSetShader(ds.Get(), nullptr, 0);
            context->IASetInputLayout(layout.Get());
            context->IASetPrimitiveTopology(topology);
            context->RSSetState(raster.Get());
            context->RSSetViewports(viewportCount, viewports);
            context->OMSetDepthStencilState(depth.Get(), stencilReference);
            context->OMSetBlendState(blend.Get(), blendFactor, sampleMask);
        }
        DeferredLightingState(const DeferredLightingState&) = delete;
        DeferredLightingState& operator=(const DeferredLightingState&) = delete;
    };
} // namespace Spark::Graphics
#endif
