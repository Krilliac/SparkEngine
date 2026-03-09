/**
 * @file TransparencySorting.h
 * @brief Transparency sorting and order-independent transparency support
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#endif

#include <vector>
#include <algorithm>
#include <cstdint>
#include <string>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace Spark::Graphics
{

    enum class TransparencyMode
    {
        Sorted,          ///< Back-to-front sorting (traditional)
        WeightedBlended, ///< Weighted Blended OIT (McGuire 2013)
        DepthPeeling     ///< Depth peeling (exact, multi-pass)
    };

    struct TransparentObject
    {
        uint32_t objectIndex = 0;
        float distanceToCamera = 0.0f;
        uint32_t renderQueue = 3000;
        float sortKey = 0.0f; ///< Combined sort key for stable sorting
    };

    struct TransparencySettings
    {
        TransparencyMode mode = TransparencyMode::Sorted;
        uint32_t maxDepthPeelingLayers = 4;
        bool enableAlphaTest = true;
        float alphaCutoff = 0.5f;
        bool premultipliedAlpha = false;
    };

    struct TransparencyMetrics
    {
        uint32_t transparentObjects = 0;
        uint32_t sortedDrawCalls = 0;
        float sortTimeMs = 0.0f;
    };

    /**
     * @class TransparencySortingSystem
     * @brief Manages transparent object sorting and rendering order
     */
    class TransparencySortingSystem
    {
      public:
        TransparencySortingSystem() = default;
        ~TransparencySortingSystem() = default;

        bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context, uint32_t width, uint32_t height)
        {
            m_device = device;
            m_context = context;
            m_width = width;
            m_height = height;

            CreateBlendStates();
            CreateWBOITTargets();

            m_initialized = true;
            return true;
        }

        void Shutdown()
        {
            m_alphaBlendState.Reset();
            m_additiveBlendState.Reset();
            m_noColorWriteState.Reset();
            m_wboitAccumTexture.Reset();
            m_wboitAccumRTV.Reset();
            m_wboitAccumSRV.Reset();
            m_wboitRevealTexture.Reset();
            m_wboitRevealRTV.Reset();
            m_wboitRevealSRV.Reset();
            m_initialized = false;
        }

        /**
         * @brief Sort transparent objects back-to-front relative to camera
         */
        void SortObjects(std::vector<TransparentObject>& objects, const XMFLOAT3& cameraPosition)
        {
            auto startTime = std::chrono::high_resolution_clock::now();

            for (auto& obj : objects)
            {
                // Sort key combines render queue and distance for stable sorting
                obj.sortKey = static_cast<float>(obj.renderQueue) * 1000.0f + obj.distanceToCamera;
            }

            // Back-to-front sort (farthest first)
            std::stable_sort(objects.begin(), objects.end(),
                             [](const TransparentObject& a, const TransparentObject& b)
                             {
                                 // First by render queue, then by distance (back to front)
                                 if (a.renderQueue != b.renderQueue)
                                     return a.renderQueue < b.renderQueue;
                                 return a.distanceToCamera > b.distanceToCamera;
                             });

            auto endTime = std::chrono::high_resolution_clock::now();
            m_metrics.sortTimeMs =
                std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime).count() / 1000.0f;
            m_metrics.transparentObjects = static_cast<uint32_t>(objects.size());
        }

        /**
         * @brief Begin weighted blended OIT accumulation pass
         */
        void BeginWBOIT()
        {
            if (!m_initialized || m_settings.mode != TransparencyMode::WeightedBlended)
                return;

            // Clear accum to (0,0,0,0) and reveal to (1,1,1,1)
            float clearAccum[] = {0, 0, 0, 0};
            float clearReveal[] = {1, 1, 1, 1};
            m_context->ClearRenderTargetView(m_wboitAccumRTV.Get(), clearAccum);
            m_context->ClearRenderTargetView(m_wboitRevealRTV.Get(), clearReveal);

            // Bind both accumulation targets
            ID3D11RenderTargetView* rtvs[] = {m_wboitAccumRTV.Get(), m_wboitRevealRTV.Get()};
            m_context->OMSetRenderTargets(2, rtvs, nullptr);

            // Set additive blend for accumulation
            float blendFactor[] = {0, 0, 0, 0};
            m_context->OMSetBlendState(m_wboitBlendState.Get(), blendFactor, 0xFFFFFFFF);
        }

        /**
         * @brief End WBOIT and composite result
         */
        void EndWBOIT(ID3D11RenderTargetView* outputRTV)
        {
            if (!m_initialized)
                return;

            m_context->OMSetRenderTargets(1, &outputRTV, nullptr);

            // Composite WBOIT: color = accum.rgb / max(accum.a, 1e-5) * (1 - reveal)
            // This requires a composite shader pass (simplified here)
        }

        /** @brief Set alpha blend state for traditional sorted transparency */
        void SetAlphaBlendState()
        {
            float blendFactor[] = {0, 0, 0, 0};
            m_context->OMSetBlendState(m_alphaBlendState.Get(), blendFactor, 0xFFFFFFFF);
        }

        /** @brief Set additive blend state */
        void SetAdditiveBlendState()
        {
            float blendFactor[] = {0, 0, 0, 0};
            m_context->OMSetBlendState(m_additiveBlendState.Get(), blendFactor, 0xFFFFFFFF);
        }

        /** @brief Restore default (no blend) state */
        void RestoreDefaultBlendState() { m_context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF); }

        TransparencySettings& GetSettings() { return m_settings; }
        const TransparencyMetrics& GetMetrics() const { return m_metrics; }

        std::string Console_GetStatus() const
        {
            std::string s = "Transparency:\n";
            s += "  Mode: ";
            switch (m_settings.mode)
            {
            case TransparencyMode::Sorted:
                s += "Back-to-Front Sorted";
                break;
            case TransparencyMode::WeightedBlended:
                s += "Weighted Blended OIT";
                break;
            case TransparencyMode::DepthPeeling:
                s += "Depth Peeling";
                break;
            }
            s += "\n  Objects: " + std::to_string(m_metrics.transparentObjects);
            s += "\n  Sort time: " + std::to_string(m_metrics.sortTimeMs) + "ms\n";
            return s;
        }

      private:
        void CreateBlendStates()
        {
            // Standard alpha blend
            D3D11_BLEND_DESC blendDesc = {};
            blendDesc.RenderTarget[0].BlendEnable = TRUE;
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            m_device->CreateBlendState(&blendDesc, &m_alphaBlendState);

            // Additive blend
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            m_device->CreateBlendState(&blendDesc, &m_additiveBlendState);

            // WBOIT blend (additive for accum, multiply for reveal)
            blendDesc.IndependentBlendEnable = TRUE;
            // RT0: Accumulation (additive, premultiplied)
            blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
            blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
            // RT1: Reveal (zero, 1-src)
            blendDesc.RenderTarget[1].BlendEnable = TRUE;
            blendDesc.RenderTarget[1].SrcBlend = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[1].DestBlend = D3D11_BLEND_INV_SRC_COLOR;
            blendDesc.RenderTarget[1].BlendOp = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[1].SrcBlendAlpha = D3D11_BLEND_ZERO;
            blendDesc.RenderTarget[1].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            blendDesc.RenderTarget[1].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            blendDesc.RenderTarget[1].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            m_device->CreateBlendState(&blendDesc, &m_wboitBlendState);
        }

        void CreateWBOITTargets()
        {
            D3D11_TEXTURE2D_DESC desc = {};
            desc.Width = m_width;
            desc.Height = m_height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

            // Accumulation texture
            m_device->CreateTexture2D(&desc, nullptr, &m_wboitAccumTexture);
            if (m_wboitAccumTexture)
            {
                m_device->CreateRenderTargetView(m_wboitAccumTexture.Get(), nullptr, &m_wboitAccumRTV);
                m_device->CreateShaderResourceView(m_wboitAccumTexture.Get(), nullptr, &m_wboitAccumSRV);
            }

            // Reveal texture (single channel)
            desc.Format = DXGI_FORMAT_R8_UNORM;
            m_device->CreateTexture2D(&desc, nullptr, &m_wboitRevealTexture);
            if (m_wboitRevealTexture)
            {
                m_device->CreateRenderTargetView(m_wboitRevealTexture.Get(), nullptr, &m_wboitRevealRTV);
                m_device->CreateShaderResourceView(m_wboitRevealTexture.Get(), nullptr, &m_wboitRevealSRV);
            }
        }

        bool m_initialized = false;
        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        uint32_t m_width = 1920, m_height = 1080;
        TransparencySettings m_settings;
        TransparencyMetrics m_metrics;

        // Blend states
        ComPtr<ID3D11BlendState> m_alphaBlendState;
        ComPtr<ID3D11BlendState> m_additiveBlendState;
        ComPtr<ID3D11BlendState> m_noColorWriteState;
        ComPtr<ID3D11BlendState> m_wboitBlendState;

        // WBOIT targets
        ComPtr<ID3D11Texture2D> m_wboitAccumTexture;
        ComPtr<ID3D11RenderTargetView> m_wboitAccumRTV;
        ComPtr<ID3D11ShaderResourceView> m_wboitAccumSRV;
        ComPtr<ID3D11Texture2D> m_wboitRevealTexture;
        ComPtr<ID3D11RenderTargetView> m_wboitRevealRTV;
        ComPtr<ID3D11ShaderResourceView> m_wboitRevealSRV;
    };

} // namespace Spark::Graphics
