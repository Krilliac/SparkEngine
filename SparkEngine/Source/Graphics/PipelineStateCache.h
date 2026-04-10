/**
 * @file PipelineStateCache.h
 * @brief Hash-based caching of D3D11 pipeline state objects
 * @author Spark Engine Team
 * @date 2026
 *
 * Caches D3D11 rasterizer, blend, depth-stencil, and sampler state
 * objects by descriptor hash, ensuring identical configurations share the
 * same state object and avoiding redundant CreateXxxState() calls.
 *
 * Also provides dirty-flag state tracking to skip redundant
 * state-setting calls on the device context.
 *
 * ## Usage
 * @code
 *   PipelineStateCache cache;
 *   cache.Initialize(device);
 *
 *   // Get or create a rasterizer state
 *   D3D11_RASTERIZER_DESC desc = {};
 *   desc.FillMode = D3D11_FILL_SOLID;
 *   desc.CullMode = D3D11_CULL_BACK;
 *   auto* rs = cache.GetRasterizerState(desc);
 *
 *   // Bind with dirty tracking
 *   cache.BindRasterizerState(context, desc);   // actually binds
 *   cache.BindRasterizerState(context, desc);   // skipped (already bound)
 * @endcode
 *
 * @see GraphicsEngine, MaterialSystem, DrawSortKey
 *
 * @note **Intentional reusable graphics utility** — Hash-based cache for D3D11 rasterizer/blend/depth-stencil/sampler state objects (`D3D11PipelineStateCache`). **Do not confuse with `Graphics/RHI/PipelineStateCache.h`** — that one defines a different class (`PipelineStateCache`) used by the RHI lifecycle. A future D3D11 backend helper will own an instance of this cache.
 *
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#endif

#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <utility>

#ifdef SPARK_PLATFORM_WINDOWS

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    /**
     * @brief Dirty flags for tracking which pipeline state categories need flushing.
     */
    enum class DirtyFlag : uint32_t
    {
        None = 0,
        Rasterizer = 1 << 0,
        BlendState = 1 << 1,
        DepthStencil = 1 << 2,
        Sampler = 1 << 3,
        Viewport = 1 << 4,
        ScissorRect = 1 << 5,
        VertexBuffer = 1 << 6,
        IndexBuffer = 1 << 7,
        Topology = 1 << 8,
        VertexShader = 1 << 9,
        PixelShader = 1 << 10,
        ComputeShader = 1 << 11,
        All = 0xFFFFFFFF
    };

    inline DirtyFlag operator|(DirtyFlag a, DirtyFlag b)
    {
        return static_cast<DirtyFlag>(std::to_underlying(a) | std::to_underlying(b));
    }

    inline DirtyFlag operator&(DirtyFlag a, DirtyFlag b)
    {
        return static_cast<DirtyFlag>(std::to_underlying(a) & std::to_underlying(b));
    }

    inline DirtyFlag& operator|=(DirtyFlag& a, DirtyFlag b)
    {
        a = a | b;
        return a;
    }

    inline bool HasFlag(DirtyFlag flags, DirtyFlag test)
    {
        return (std::to_underlying(flags) & std::to_underlying(test)) != 0;
    }

    /**
     * @brief Cache metrics for profiling state object reuse.
     */
    struct PipelineCacheMetrics
    {
        uint32_t rasterizerCacheSize = 0;
        uint32_t blendCacheSize = 0;
        uint32_t depthStencilCacheSize = 0;
        uint32_t samplerCacheSize = 0;
        uint32_t cacheHits = 0;
        uint32_t cacheMisses = 0;
        uint32_t redundantBindsAvoided = 0;
    };

    /**
     * @brief Caches D3D11 pipeline state objects and tracks dirty state to minimize API calls.
     */
    class D3D11PipelineStateCache
    {
      public:
        D3D11PipelineStateCache() = default;
        ~D3D11PipelineStateCache() = default;

        /**
         * @brief Initialize the cache with a D3D11 device.
         */
        bool Initialize(ID3D11Device* device)
        {
            m_device = device;
            return device != nullptr;
        }

        void Shutdown()
        {
            m_rasterizerCache.clear();
            m_blendCache.clear();
            m_depthStencilCache.clear();
            m_samplerCache.clear();
            m_device = nullptr;
        }

        // =================================================================
        // Rasterizer State
        // =================================================================

        ID3D11RasterizerState* GetRasterizerState(const D3D11_RASTERIZER_DESC& desc)
        {
            uint64_t hash = HashDesc(&desc, sizeof(desc));
            auto it = m_rasterizerCache.find(hash);
            if (it != m_rasterizerCache.end())
            {
                ++m_metrics.cacheHits;
                return it->second.Get();
            }

            ++m_metrics.cacheMisses;
            ComPtr<ID3D11RasterizerState> state;
            HRESULT hr = m_device->CreateRasterizerState(&desc, &state);
            if (FAILED(hr))
                return nullptr;

            auto* raw = state.Get();
            m_rasterizerCache[hash] = std::move(state);
            m_metrics.rasterizerCacheSize = static_cast<uint32_t>(m_rasterizerCache.size());
            return raw;
        }

        /**
         * @brief Bind rasterizer state only if it differs from the currently bound one.
         */
        void BindRasterizerState(ID3D11DeviceContext* context, const D3D11_RASTERIZER_DESC& desc)
        {
            uint64_t hash = HashDesc(&desc, sizeof(desc));
            if (hash == m_boundRasterizerHash)
            {
                ++m_metrics.redundantBindsAvoided;
                return;
            }

            auto* state = GetRasterizerState(desc);
            if (state)
            {
                context->RSSetState(state);
                m_boundRasterizerHash = hash;
            }
        }

        // =================================================================
        // Blend State
        // =================================================================

        ID3D11BlendState* GetBlendState(const D3D11_BLEND_DESC& desc)
        {
            uint64_t hash = HashDesc(&desc, sizeof(desc));
            auto it = m_blendCache.find(hash);
            if (it != m_blendCache.end())
            {
                ++m_metrics.cacheHits;
                return it->second.Get();
            }

            ++m_metrics.cacheMisses;
            ComPtr<ID3D11BlendState> state;
            HRESULT hr = m_device->CreateBlendState(&desc, &state);
            if (FAILED(hr))
                return nullptr;

            auto* raw = state.Get();
            m_blendCache[hash] = std::move(state);
            m_metrics.blendCacheSize = static_cast<uint32_t>(m_blendCache.size());
            return raw;
        }

        void BindBlendState(ID3D11DeviceContext* context, const D3D11_BLEND_DESC& desc,
                            const float blendFactor[4] = nullptr, uint32_t sampleMask = 0xFFFFFFFF)
        {
            uint64_t hash = HashDesc(&desc, sizeof(desc));
            if (hash == m_boundBlendHash)
            {
                ++m_metrics.redundantBindsAvoided;
                return;
            }

            auto* state = GetBlendState(desc);
            if (state)
            {
                context->OMSetBlendState(state, blendFactor, sampleMask);
                m_boundBlendHash = hash;
            }
        }

        // =================================================================
        // Depth-Stencil State
        // =================================================================

        ID3D11DepthStencilState* GetDepthStencilState(const D3D11_DEPTH_STENCIL_DESC& desc)
        {
            uint64_t hash = HashDesc(&desc, sizeof(desc));
            auto it = m_depthStencilCache.find(hash);
            if (it != m_depthStencilCache.end())
            {
                ++m_metrics.cacheHits;
                return it->second.Get();
            }

            ++m_metrics.cacheMisses;
            ComPtr<ID3D11DepthStencilState> state;
            HRESULT hr = m_device->CreateDepthStencilState(&desc, &state);
            if (FAILED(hr))
                return nullptr;

            auto* raw = state.Get();
            m_depthStencilCache[hash] = std::move(state);
            m_metrics.depthStencilCacheSize = static_cast<uint32_t>(m_depthStencilCache.size());
            return raw;
        }

        void BindDepthStencilState(ID3D11DeviceContext* context, const D3D11_DEPTH_STENCIL_DESC& desc,
                                   uint32_t stencilRef = 0)
        {
            uint64_t hash = HashDesc(&desc, sizeof(desc));
            if (hash == m_boundDepthStencilHash)
            {
                ++m_metrics.redundantBindsAvoided;
                return;
            }

            auto* state = GetDepthStencilState(desc);
            if (state)
            {
                context->OMSetDepthStencilState(state, stencilRef);
                m_boundDepthStencilHash = hash;
            }
        }

        // =================================================================
        // Sampler State
        // =================================================================

        ID3D11SamplerState* GetSamplerState(const D3D11_SAMPLER_DESC& desc)
        {
            uint64_t hash = HashDesc(&desc, sizeof(desc));
            auto it = m_samplerCache.find(hash);
            if (it != m_samplerCache.end())
            {
                ++m_metrics.cacheHits;
                return it->second.Get();
            }

            ++m_metrics.cacheMisses;
            ComPtr<ID3D11SamplerState> state;
            HRESULT hr = m_device->CreateSamplerState(&desc, &state);
            if (FAILED(hr))
                return nullptr;

            auto* raw = state.Get();
            m_samplerCache[hash] = std::move(state);
            m_metrics.samplerCacheSize = static_cast<uint32_t>(m_samplerCache.size());
            return raw;
        }

        // =================================================================
        // Frame Management
        // =================================================================

        /**
         * @brief Reset all bound-state tracking for a new frame.
         *
         * Call at the start of each frame to ensure the first bind in each
         * category always goes through (since the context state is unknown
         * after Present()).
         */
        void ResetBoundState()
        {
            m_boundRasterizerHash = 0;
            m_boundBlendHash = 0;
            m_boundDepthStencilHash = 0;
            m_dirtyFlags = DirtyFlag::All;
        }

        /**
         * @brief Mark specific state categories as dirty.
         */
        void MarkDirty(DirtyFlag flags) { m_dirtyFlags |= flags; }

        /**
         * @brief Check if a state category is dirty.
         */
        bool IsDirty(DirtyFlag flag) const { return HasFlag(m_dirtyFlags, flag); }

        /**
         * @brief Clear dirty flag after flushing state.
         */
        void ClearDirty(DirtyFlag flag)
        {
            m_dirtyFlags = static_cast<DirtyFlag>(static_cast<uint32_t>(m_dirtyFlags) & ~static_cast<uint32_t>(flag));
        }

        // =================================================================
        // Metrics
        // =================================================================

        const PipelineCacheMetrics& GetMetrics() const { return m_metrics; }

        void ResetMetrics()
        {
            m_metrics.cacheHits = 0;
            m_metrics.cacheMisses = 0;
            m_metrics.redundantBindsAvoided = 0;
        }

        std::string Console_GetStatus() const
        {
            std::string s = "Pipeline State Cache:\n";
            s += "  Rasterizer states: " + std::to_string(m_metrics.rasterizerCacheSize) + "\n";
            s += "  Blend states:      " + std::to_string(m_metrics.blendCacheSize) + "\n";
            s += "  DepthStencil:      " + std::to_string(m_metrics.depthStencilCacheSize) + "\n";
            s += "  Sampler states:    " + std::to_string(m_metrics.samplerCacheSize) + "\n";
            s += "  Cache hits:        " + std::to_string(m_metrics.cacheHits) + "\n";
            s += "  Cache misses:      " + std::to_string(m_metrics.cacheMisses) + "\n";
            s += "  Redundant skipped: " + std::to_string(m_metrics.redundantBindsAvoided) + "\n";
            return s;
        }

      private:
        /**
         * @brief FNV-1a hash over raw bytes of a descriptor struct.
         */
        static uint64_t HashDesc(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const uint8_t*>(data);
            uint64_t hash = 14695981039346656037ULL;
            for (size_t i = 0; i < size; ++i)
            {
                hash ^= static_cast<uint64_t>(bytes[i]);
                hash *= 1099511628211ULL;
            }
            return hash;
        }

        ID3D11Device* m_device = nullptr;

        // State caches (hash -> COM object)
        std::unordered_map<uint64_t, ComPtr<ID3D11RasterizerState>> m_rasterizerCache;
        std::unordered_map<uint64_t, ComPtr<ID3D11BlendState>> m_blendCache;
        std::unordered_map<uint64_t, ComPtr<ID3D11DepthStencilState>> m_depthStencilCache;
        std::unordered_map<uint64_t, ComPtr<ID3D11SamplerState>> m_samplerCache;

        // Currently bound state hashes (for redundancy elimination)
        uint64_t m_boundRasterizerHash = 0;
        uint64_t m_boundBlendHash = 0;
        uint64_t m_boundDepthStencilHash = 0;

        // Dirty flags for state categories
        DirtyFlag m_dirtyFlags = DirtyFlag::All;

        PipelineCacheMetrics m_metrics{};
    };

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
