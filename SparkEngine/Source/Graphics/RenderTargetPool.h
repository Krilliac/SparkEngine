/**
 * @file RenderTargetPool.h
 * @brief Pooled transient render target management
 * @author Spark Engine Team
 * @date 2026
 *
 * Manages a pool of
 * render targets indexed by (format, width, height, sampleCount), recycling
 * targets whose lifetimes don't overlap across frames. Eliminates redundant
 * CreateTexture2D/Release calls for transient render targets used in
 * post-processing chains, shadow maps, and G-buffer passes.
 *
 * Unused targets are reclaimed after a configurable number of idle frames
 * (default 60) to avoid VRAM waste.
 *
 * ## Usage
 * @code
 *   RenderTargetPool pool;
 *   pool.Initialize(device);
 *
 *   RenderTargetDesc desc;
 *   desc.width = 1920;
 *   desc.height = 1080;
 *   desc.format = DXGI_FORMAT_R16G16B16A16_FLOAT;
 *
 *   // Acquire a target for this frame
 *   auto handle = pool.Acquire(desc);
 *
 *   // Use it...
 *   auto* rtv = pool.GetRTV(handle);
 *   auto* srv = pool.GetSRV(handle);
 *
 *   // Release back to pool at end of pass
 *   pool.Release(handle);
 *
 *   // Call once per frame to reclaim stale targets
 *   pool.Tick();
 * @endcode
 *
 * @see RenderGraph, GraphicsEngine
 *
 * @note **Intentional reusable graphics utility** — Pooled render target allocator with acquire/release semantics. A render graph will instantiate one of these per device and reclaim RTs between passes. No singleton — the pool is per-device.
 *
 */

#pragma once

#include "../Core/Platform.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <d3d11.h>
#include <wrl/client.h>
#include <dxgiformat.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

#ifdef SPARK_PLATFORM_WINDOWS

using Microsoft::WRL::ComPtr;

namespace Spark::Graphics
{

    /**
     * @brief Descriptor for a pooled render target.
     */
    struct RenderTargetDesc
    {
        uint32_t width = 0;
        uint32_t height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
        uint32_t sampleCount = 1;
        uint32_t mipLevels = 1;
        bool isDepthStencil = false;
        std::string debugName; ///< Optional debug name for PIX/RenderDoc

        bool operator==(const RenderTargetDesc& other) const
        {
            return width == other.width && height == other.height && format == other.format &&
                   sampleCount == other.sampleCount && mipLevels == other.mipLevels &&
                   isDepthStencil == other.isDepthStencil;
        }
    };

    /**
     * @brief Opaque handle to a pooled render target.
     */
    struct PooledRTHandle
    {
        uint32_t index = UINT32_MAX;
        bool IsValid() const { return index != UINT32_MAX; }
    };

    /**
     * @brief Pooled render target metrics.
     */
    struct RenderTargetPoolMetrics
    {
        uint32_t totalTargets = 0;     ///< Total targets in pool
        uint32_t activeTargets = 0;    ///< Currently acquired targets
        uint32_t availableTargets = 0; ///< Available for reuse
        uint32_t reclaimedTargets = 0; ///< Targets reclaimed this lifetime
        size_t totalMemoryBytes = 0;   ///< Estimated VRAM usage
    };

    /**
     * @brief Render target pool that recycles transient GPU targets by descriptor.
     */
    class RenderTargetPool
    {
      public:
        RenderTargetPool() = default;
        ~RenderTargetPool() = default;

        bool Initialize(ID3D11Device* device, uint32_t reclaimAfterFrames = 60)
        {
            m_device = device;
            m_reclaimAfterFrames = reclaimAfterFrames;
            return device != nullptr;
        }

        void Shutdown()
        {
            m_entries.clear();
            m_device = nullptr;
        }

        /**
         * @brief Acquire a render target matching the descriptor.
         *
         * Searches available (released) targets for a matching descriptor.
         * If none found, creates a new one. Returns a handle for access.
         */
        PooledRTHandle Acquire(const RenderTargetDesc& desc)
        {
            // Search for a matching available entry
            for (uint32_t i = 0; i < static_cast<uint32_t>(m_entries.size()); ++i)
            {
                auto& entry = m_entries[i];
                if (!entry.inUse && entry.desc == desc)
                {
                    entry.inUse = true;
                    entry.idleFrames = 0;
                    return {i};
                }
            }

            // No match found — create a new target
            PoolEntry entry;
            entry.desc = desc;
            entry.inUse = true;
            entry.idleFrames = 0;

            if (!CreateTarget(entry))
            {
                return {}; // invalid handle
            }

            uint32_t index = static_cast<uint32_t>(m_entries.size());
            m_entries.push_back(std::move(entry));
            return {index};
        }

        /**
         * @brief Release a render target back to the pool for reuse.
         */
        void Release(PooledRTHandle handle)
        {
            if (!handle.IsValid() || handle.index >= m_entries.size())
                return;

            m_entries[handle.index].inUse = false;
            m_entries[handle.index].idleFrames = 0;
        }

        /**
         * @brief Call once per frame to age and reclaim unused targets.
         */
        void Tick()
        {
            ++m_frameCounter;

            for (auto it = m_entries.begin(); it != m_entries.end();)
            {
                if (!it->inUse)
                {
                    ++it->idleFrames;
                    if (it->idleFrames >= m_reclaimAfterFrames)
                    {
                        ++m_totalReclaimed;
                        it = m_entries.erase(it);
                        continue;
                    }
                }
                ++it;
            }
        }

        // =================================================================
        // Accessors
        // =================================================================

        ID3D11RenderTargetView* GetRTV(PooledRTHandle handle) const
        {
            if (!handle.IsValid() || handle.index >= m_entries.size())
                return nullptr;
            return m_entries[handle.index].rtv.Get();
        }

        ID3D11DepthStencilView* GetDSV(PooledRTHandle handle) const
        {
            if (!handle.IsValid() || handle.index >= m_entries.size())
                return nullptr;
            return m_entries[handle.index].dsv.Get();
        }

        ID3D11ShaderResourceView* GetSRV(PooledRTHandle handle) const
        {
            if (!handle.IsValid() || handle.index >= m_entries.size())
                return nullptr;
            return m_entries[handle.index].srv.Get();
        }

        ID3D11Texture2D* GetTexture(PooledRTHandle handle) const
        {
            if (!handle.IsValid() || handle.index >= m_entries.size())
                return nullptr;
            return m_entries[handle.index].texture.Get();
        }

        const RenderTargetDesc& GetDesc(PooledRTHandle handle) const { return m_entries[handle.index].desc; }

        // =================================================================
        // Metrics
        // =================================================================

        RenderTargetPoolMetrics GetMetrics() const
        {
            RenderTargetPoolMetrics m;
            m.totalTargets = static_cast<uint32_t>(m_entries.size());
            m.reclaimedTargets = m_totalReclaimed;
            m.totalMemoryBytes = 0;

            for (const auto& e : m_entries)
            {
                if (e.inUse)
                    ++m.activeTargets;
                else
                    ++m.availableTargets;
                m.totalMemoryBytes += EstimateMemory(e.desc);
            }

            return m;
        }

        std::string Console_GetStatus() const
        {
            auto m = GetMetrics();
            std::string s = "Render Target Pool:\n";
            s += "  Total:     " + std::to_string(m.totalTargets) + "\n";
            s += "  Active:    " + std::to_string(m.activeTargets) + "\n";
            s += "  Available: " + std::to_string(m.availableTargets) + "\n";
            s += "  Reclaimed: " + std::to_string(m.reclaimedTargets) + "\n";
            s += "  VRAM est.: " + std::to_string(m.totalMemoryBytes / (1024 * 1024)) + " MB\n";
            return s;
        }

      private:
        struct PoolEntry
        {
            RenderTargetDesc desc;
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11DepthStencilView> dsv;
            ComPtr<ID3D11ShaderResourceView> srv;
            bool inUse = false;
            uint32_t idleFrames = 0;
        };

        bool CreateTarget(PoolEntry& entry)
        {
            D3D11_TEXTURE2D_DESC texDesc = {};
            texDesc.Width = entry.desc.width;
            texDesc.Height = entry.desc.height;
            texDesc.MipLevels = entry.desc.mipLevels;
            texDesc.ArraySize = 1;
            texDesc.Format = entry.desc.format;
            texDesc.SampleDesc.Count = entry.desc.sampleCount;
            texDesc.SampleDesc.Quality = 0;
            texDesc.Usage = D3D11_USAGE_DEFAULT;

            if (entry.desc.isDepthStencil)
            {
                // For depth: use typeless format for SRV compatibility
                DXGI_FORMAT typeless = entry.desc.format;
                DXGI_FORMAT srvFormat = entry.desc.format;

                if (entry.desc.format == DXGI_FORMAT_D32_FLOAT)
                {
                    typeless = DXGI_FORMAT_R32_TYPELESS;
                    srvFormat = DXGI_FORMAT_R32_FLOAT;
                }
                else if (entry.desc.format == DXGI_FORMAT_D24_UNORM_S8_UINT)
                {
                    typeless = DXGI_FORMAT_R24G8_TYPELESS;
                    srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
                }
                else if (entry.desc.format == DXGI_FORMAT_D16_UNORM)
                {
                    typeless = DXGI_FORMAT_R16_TYPELESS;
                    srvFormat = DXGI_FORMAT_R16_UNORM;
                }

                texDesc.Format = typeless;
                texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &entry.texture);
                if (FAILED(hr))
                    return false;

                D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
                dsvDesc.Format = entry.desc.format;
                dsvDesc.ViewDimension =
                    entry.desc.sampleCount > 1 ? D3D11_DSV_DIMENSION_TEXTURE2DMS : D3D11_DSV_DIMENSION_TEXTURE2D;
                hr = m_device->CreateDepthStencilView(entry.texture.Get(), &dsvDesc, &entry.dsv);
                if (FAILED(hr))
                    return false;

                D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
                srvDesc.Format = srvFormat;
                srvDesc.ViewDimension =
                    entry.desc.sampleCount > 1 ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
                srvDesc.Texture2D.MipLevels = entry.desc.mipLevels;
                hr = m_device->CreateShaderResourceView(entry.texture.Get(), &srvDesc, &entry.srv);
                if (FAILED(hr))
                    return false;
            }
            else
            {
                texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

                HRESULT hr = m_device->CreateTexture2D(&texDesc, nullptr, &entry.texture);
                if (FAILED(hr))
                    return false;

                hr = m_device->CreateRenderTargetView(entry.texture.Get(), nullptr, &entry.rtv);
                if (FAILED(hr))
                    return false;

                hr = m_device->CreateShaderResourceView(entry.texture.Get(), nullptr, &entry.srv);
                if (FAILED(hr))
                    return false;
            }

            return true;
        }

        static size_t EstimateMemory(const RenderTargetDesc& desc)
        {
            size_t bpp = 4; // default 4 bytes per pixel
            switch (desc.format)
            {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                bpp = 8;
                break;
            case DXGI_FORMAT_R32G32B32A32_FLOAT:
                bpp = 16;
                break;
            case DXGI_FORMAT_R11G11B10_FLOAT:
                bpp = 4;
                break;
            case DXGI_FORMAT_R16G16_FLOAT:
                bpp = 4;
                break;
            case DXGI_FORMAT_R32_FLOAT:
            case DXGI_FORMAT_D32_FLOAT:
                bpp = 4;
                break;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
                bpp = 4;
                break;
            default:
                bpp = 4;
                break;
            }
            return static_cast<size_t>(desc.width) * desc.height * bpp * desc.sampleCount;
        }

        ID3D11Device* m_device = nullptr;
        std::vector<PoolEntry> m_entries;
        uint32_t m_reclaimAfterFrames = 60;
        uint32_t m_frameCounter = 0;
        uint32_t m_totalReclaimed = 0;
    };

} // namespace Spark::Graphics

#endif // SPARK_PLATFORM_WINDOWS
