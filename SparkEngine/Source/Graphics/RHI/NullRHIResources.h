/**
 * @file NullRHIResources.h
 * @brief Concrete stub IRHI* resource implementations for the headless backend
 *
 * Phase Y Theme 3B activation. Previously `NullRHIDevice::CreateBuffer` and
 * friends returned `nullptr`, making the headless backend unusable for any
 * consumer that checked `buffer != nullptr`. This header ships real stub
 * resource classes so:
 *
 *   1. `NullRHIDevice` can wire `Spark::RHI::HandlePool<T, Tag>` for each
 *      resource type — the pool registers real pointers, not nullptr
 *      sentinels.
 *   2. `NullRHIDevice` can wire `Spark::RHI::TransientBufferAllocator` into
 *      its frame lifecycle — the allocator needs buffers whose `MapBuffer`
 *      call returns a writable CPU pointer.
 *   3. Headless CI tests can exercise any code path that consumes
 *      `IRHIBuffer*` without crashing on null.
 *
 * None of these stubs touch a GPU. `NullBuffer` owns a `std::vector<uint8_t>`
 * for CPU-side storage; every other class is interface-only.
 */

#pragma once

#include "RHIResources.h"
#include "RHITypes.h"

#include <cstring>
#include <string>
#include <vector>

namespace Spark
{
    namespace RHI
    {

        /**
         * @brief CPU-backed stub buffer. Owns a std::vector that MapBuffer returns.
         */
        class NullBuffer : public IRHIBuffer
        {
          public:
            explicit NullBuffer(const RHIBufferDesc& desc)
                : m_desc(desc), m_storage(static_cast<size_t>(desc.size), 0), m_debugName(desc.debugName)
            {
            }

            const std::string& GetDebugName() const override { return m_debugName; }
            void SetDebugName(const std::string& name) override { m_debugName = name; }
            bool IsValid() const override { return true; }

            const RHIBufferDesc& GetDesc() const override { return m_desc; }
            uint64_t GetSize() const override { return m_desc.size; }
            uint32_t GetStride() const override { return m_desc.stride; }
            void* GetNativeHandle() const override { return nullptr; }

            /** @brief CPU pointer used by NullRHIDevice::MapBuffer. */
            void* GetCpuPointer() { return m_storage.empty() ? nullptr : m_storage.data(); }

            bool Write(const void* data, size_t size, size_t offset)
            {
                if ((!data && size != 0) || offset > m_storage.size() || size > m_storage.size() - offset)
                    return false;
                if (size != 0)
                    std::memcpy(m_storage.data() + offset, data, size);
                return true;
            }

          private:
            RHIBufferDesc m_desc;
            std::vector<uint8_t> m_storage;
            std::string m_debugName;
        };

        /**
         * @brief Interface-only stub texture. No backing memory.
         */
        class NullTexture : public IRHITexture
        {
          public:
            explicit NullTexture(const RHITextureDesc& desc) : m_desc(desc), m_debugName(desc.debugName) {}

            const std::string& GetDebugName() const override { return m_debugName; }
            void SetDebugName(const std::string& name) override { m_debugName = name; }
            bool IsValid() const override { return true; }

            const RHITextureDesc& GetDesc() const override { return m_desc; }
            uint32_t GetWidth() const override { return m_desc.width; }
            uint32_t GetHeight() const override { return m_desc.height; }
            uint32_t GetDepth() const override { return m_desc.depth; }
            uint32_t GetMipLevels() const override { return m_desc.mipLevels; }
            PixelFormat GetFormat() const override { return m_desc.format; }
            void* GetNativeHandle() const override { return nullptr; }
            void* GetShaderResourceView() const override { return nullptr; }
            void* GetRenderTargetView() const override { return nullptr; }
            void* GetDepthStencilView() const override { return nullptr; }

          private:
            RHITextureDesc m_desc;
            std::string m_debugName;
        };

        /**
         * @brief Interface-only stub shader.
         */
        class NullShader : public IRHIShader
        {
          public:
            explicit NullShader(const RHIShaderDesc& desc) : m_desc(desc), m_debugName(desc.debugName) {}

            const std::string& GetDebugName() const override { return m_debugName; }
            void SetDebugName(const std::string& name) override { m_debugName = name; }
            bool IsValid() const override { return true; }

            RHIShaderStage GetStage() const override { return m_desc.stage; }
            const std::string& GetEntryPoint() const override { return m_desc.entryPoint; }
            void* GetNativeHandle() const override { return nullptr; }
            const void* GetBytecode() const override { return m_desc.bytecode; }
            size_t GetBytecodeSize() const override { return m_desc.bytecodeSize; }

          private:
            RHIShaderDesc m_desc;
            std::string m_debugName;
        };

        /**
         * @brief Interface-only stub sampler.
         */
        class NullSampler : public IRHISampler
        {
          public:
            explicit NullSampler(const RHISamplerDesc& desc) : m_desc(desc) {}

            const std::string& GetDebugName() const override { return m_debugName; }
            void SetDebugName(const std::string& name) override { m_debugName = name; }
            bool IsValid() const override { return true; }

            const RHISamplerDesc& GetDesc() const override { return m_desc; }
            void* GetNativeHandle() const override { return nullptr; }

          private:
            RHISamplerDesc m_desc;
            std::string m_debugName;
        };

        /**
         * @brief Interface-only stub pipeline state.
         */
        class NullPipelineState : public IRHIPipelineState
        {
          public:
            explicit NullPipelineState(const RHIPipelineStateDesc& desc) : m_desc(desc) {}

            const std::string& GetDebugName() const override { return m_debugName; }
            void SetDebugName(const std::string& name) override { m_debugName = name; }
            bool IsValid() const override { return true; }

            const RHIPipelineStateDesc& GetDesc() const override { return m_desc; }
            void* GetNativeHandle() const override { return nullptr; }

          private:
            RHIPipelineStateDesc m_desc;
            std::string m_debugName;
        };

    } // namespace RHI
} // namespace Spark
