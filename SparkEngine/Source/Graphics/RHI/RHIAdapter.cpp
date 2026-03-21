/**
 * @file RHIAdapter.cpp
 * @brief Implementation of the RHI adapter layer (R3.2 -- Route rendering through RHI)
 * @author Spark Engine Team
 * @date 2026
 */

#include "RHIAdapter.h"
#include "../../Utils/Validate.h"

#include <algorithm>
#include <cassert>

namespace Spark::RHI
{

    // ========================================================================
    // Lifecycle
    // ========================================================================

    RHIAdapter::~RHIAdapter()
    {
        if (m_device)
        {
            Shutdown();
        }
    }

    bool RHIAdapter::Initialize(IRHIDevice* device)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, device);
        if (!device)
        {
            return false;
        }

        m_device = device;
        m_commandList = device->GetImmediateCommandList();
        return m_commandList != nullptr;
    }

    void RHIAdapter::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Graphics);
        SPARK_LOG_INFO(Spark::LogCategory::Graphics, "RHIAdapter::Shutdown");
        if (!m_device)
        {
            return;
        }

        // Destroy all tracked resources in reverse creation order.
        for (auto* pso : m_ownedPipelineStates)
        {
            m_device->DestroyPipelineState(pso);
        }
        m_ownedPipelineStates.clear();

        for (auto* sampler : m_ownedSamplers)
        {
            m_device->DestroySampler(sampler);
        }
        m_ownedSamplers.clear();

        for (auto* shader : m_ownedShaders)
        {
            m_device->DestroyShader(shader);
        }
        m_ownedShaders.clear();

        for (auto* tex : m_ownedTextures)
        {
            m_device->DestroyTexture(tex);
        }
        m_ownedTextures.clear();

        for (auto* buf : m_ownedBuffers)
        {
            m_device->DestroyBuffer(buf);
        }
        m_ownedBuffers.clear();

        m_commandList = nullptr;
        m_device = nullptr;
        m_frameActive = false;
    }

    // ========================================================================
    // Frame bookkeeping
    // ========================================================================

    void RHIAdapter::BeginFrame()
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, m_device != nullptr,
                          "RHIAdapter::BeginFrame -- not initialized");
        SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, !m_frameActive,
                          "RHIAdapter::BeginFrame -- frame already in progress");

        m_device->BeginFrame();
        m_commandList = m_device->GetImmediateCommandList();
        m_commandList->Begin();
        m_frameActive = true;
    }

    void RHIAdapter::EndFrame()
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, m_frameActive, "RHIAdapter::EndFrame -- no frame in progress");

        m_commandList->End();
        m_device->EndFrame();
        m_frameActive = false;
    }

    // ========================================================================
    // Pass markers
    // ========================================================================

    void RHIAdapter::BeginPass(const char* name)
    {
        assert(m_commandList && "RHIAdapter::BeginPass -- no active command list");
        m_commandList->BeginEvent(name);
    }

    void RHIAdapter::EndPass()
    {
        assert(m_commandList && "RHIAdapter::EndPass -- no active command list");
        m_commandList->EndEvent();
    }

    // ========================================================================
    // Render target binding
    // ========================================================================

    void RHIAdapter::SetRenderTargets(std::span<IRHITexture* const> renderTargets, IRHITexture* depthStencil)
    {
        assert(m_commandList);

        auto count = static_cast<uint32_t>(renderTargets.size());
        m_commandList->SetRenderTargets(renderTargets.data(), count, depthStencil);
    }

    void RHIAdapter::ClearRenderTarget(IRHITexture* target, const float color[4])
    {
        assert(m_commandList);
        m_commandList->ClearRenderTarget(target, color);
    }

    void RHIAdapter::ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil)
    {
        assert(m_commandList);
        m_commandList->ClearDepthStencil(target, depth, stencil);
    }

    // ========================================================================
    // Viewport / scissor
    // ========================================================================

    void RHIAdapter::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth)
    {
        assert(m_commandList);

        RHIViewport vp{};
        vp.x = x;
        vp.y = y;
        vp.width = width;
        vp.height = height;
        vp.minDepth = minDepth;
        vp.maxDepth = maxDepth;
        m_commandList->SetViewport(vp);
    }

    void RHIAdapter::SetViewport(const AdapterViewport& vp)
    {
        SetViewport(vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth);
    }

    void RHIAdapter::SetScissorRect(const AdapterScissorRect& rect)
    {
        assert(m_commandList);

        RHIScissorRect sr{};
        sr.left = rect.left;
        sr.top = rect.top;
        sr.right = rect.right;
        sr.bottom = rect.bottom;
        m_commandList->SetScissorRect(sr);
    }

    void RHIAdapter::SetScissorRect(int32_t width, int32_t height)
    {
        AdapterScissorRect sr{};
        sr.left = 0;
        sr.top = 0;
        sr.right = width;
        sr.bottom = height;
        SetScissorRect(sr);
    }

    // ========================================================================
    // Pipeline state
    // ========================================================================

    void RHIAdapter::SetPipelineState(IRHIPipelineState* pso)
    {
        assert(m_commandList);
        m_commandList->SetPipelineState(pso);
    }

    void RHIAdapter::SetPrimitiveTopology(RHIPrimitiveTopology topology)
    {
        assert(m_commandList);
        m_commandList->SetPrimitiveTopology(topology);
    }

    // ========================================================================
    // Resource binding
    // ========================================================================

    void RHIAdapter::BindVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
    {
        assert(m_commandList);
        m_commandList->SetVertexBuffer(buffer, slot, offset);
    }

    void RHIAdapter::BindIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
    {
        assert(m_commandList);
        m_commandList->SetIndexBuffer(buffer, offset);
    }

    void RHIAdapter::BindConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer)
    {
        assert(m_commandList);
        m_commandList->SetConstantBuffer(stage, slot, buffer);
    }

    void RHIAdapter::BindTexture(RHIShaderStage stage, uint32_t slot, IRHITexture* texture)
    {
        assert(m_commandList);
        m_commandList->SetShaderResource(stage, slot, texture);
    }

    void RHIAdapter::BindSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler)
    {
        assert(m_commandList);
        m_commandList->SetSampler(stage, slot, sampler);
    }

    // ========================================================================
    // Draw commands
    // ========================================================================

    void RHIAdapter::Draw(uint32_t vertexCount, uint32_t startVertex)
    {
        assert(m_commandList);
        m_commandList->Draw(vertexCount, startVertex);
    }

    void RHIAdapter::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
    {
        assert(m_commandList);
        m_commandList->DrawIndexed(indexCount, startIndex, baseVertex);
    }

    void RHIAdapter::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                   uint32_t startInstance)
    {
        assert(m_commandList);
        m_commandList->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
    }

    void RHIAdapter::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                          int32_t baseVertex, uint32_t startInstance)
    {
        assert(m_commandList);
        m_commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
    }

    // ========================================================================
    // Compute dispatch
    // ========================================================================

    void RHIAdapter::Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        assert(m_commandList);
        m_commandList->Dispatch(groupsX, groupsY, groupsZ);
    }

    // ========================================================================
    // Indirect draw/dispatch
    // ========================================================================

    void RHIAdapter::DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
    {
        assert(m_commandList);
        m_commandList->DrawInstancedIndirect(argsBuffer, argsOffset);
    }

    void RHIAdapter::DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
    {
        assert(m_commandList);
        m_commandList->DrawIndexedInstancedIndirect(argsBuffer, argsOffset);
    }

    void RHIAdapter::DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
    {
        assert(m_commandList);
        m_commandList->DispatchIndirect(argsBuffer, argsOffset);
    }

    // ========================================================================
    // Resource creation -- buffers
    // ========================================================================

    IRHIBuffer* RHIAdapter::CreateVertexBuffer(const void* data, uint64_t size, uint32_t stride)
    {
        assert(m_device);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = stride;
        desc.usage = RHIBufferUsage::Vertex;
        desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
        desc.initialData = data;
        desc.debugName = "AdapterVB";

        IRHIBuffer* buffer = m_device->CreateBuffer(desc);
        if (buffer)
        {
            m_ownedBuffers.push_back(buffer);
        }
        return buffer;
    }

    IRHIBuffer* RHIAdapter::CreateIndexBuffer(const void* data, uint64_t size, uint32_t stride)
    {
        assert(m_device);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = stride;
        desc.usage = RHIBufferUsage::Index;
        desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
        desc.initialData = data;
        desc.debugName = "AdapterIB";

        IRHIBuffer* buffer = m_device->CreateBuffer(desc);
        if (buffer)
        {
            m_ownedBuffers.push_back(buffer);
        }
        return buffer;
    }

    IRHIBuffer* RHIAdapter::CreateConstantBuffer(uint64_t size)
    {
        assert(m_device);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = 0;
        desc.usage = RHIBufferUsage::Constant;
        desc.access = RHIBufferAccess::Dynamic;
        desc.initialData = nullptr;
        desc.debugName = "AdapterCB";

        IRHIBuffer* buffer = m_device->CreateBuffer(desc);
        if (buffer)
        {
            m_ownedBuffers.push_back(buffer);
        }
        return buffer;
    }

    IRHIBuffer* RHIAdapter::CreateStructuredBuffer(const void* data, uint64_t size, uint32_t stride)
    {
        assert(m_device);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = stride;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::Storage;
        desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
        desc.initialData = data;
        desc.debugName = "AdapterSB";

        IRHIBuffer* buffer = m_device->CreateBuffer(desc);
        if (buffer)
        {
            m_ownedBuffers.push_back(buffer);
        }
        return buffer;
    }

    // ========================================================================
    // Resource creation -- textures
    // ========================================================================

    IRHITexture* RHIAdapter::CreateTexture2D(uint32_t width, uint32_t height, PixelFormat format, RHITextureUsage usage,
                                             const void* initialData)
    {
        assert(m_device);

        RHITextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.sampleCount = 1;
        desc.format = format;
        desc.type = RHITextureType::Texture2D;
        desc.usage = usage;
        desc.debugName = "AdapterTex2D";

        IRHITexture* texture = m_device->CreateTexture(desc);
        if (texture)
        {
            if (initialData)
            {
                m_device->UpdateTexture(texture, initialData);
            }
            m_ownedTextures.push_back(texture);
        }
        return texture;
    }

    IRHITexture* RHIAdapter::CreateDepthStencil(uint32_t width, uint32_t height, PixelFormat format)
    {
        assert(m_device);

        RHITextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.sampleCount = 1;
        desc.format = format;
        desc.type = RHITextureType::Texture2D;
        desc.usage = RHITextureUsage::DepthStencil;
        desc.clearDepth = 1.0f;
        desc.clearStencil = 0;
        desc.debugName = "AdapterDepth";

        IRHITexture* texture = m_device->CreateTexture(desc);
        if (texture)
        {
            m_ownedTextures.push_back(texture);
        }
        return texture;
    }

    IRHITexture* RHIAdapter::CreateRenderTarget(uint32_t width, uint32_t height, PixelFormat format)
    {
        assert(m_device);

        RHITextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.depth = 1;
        desc.mipLevels = 1;
        desc.arraySize = 1;
        desc.sampleCount = 1;
        desc.format = format;
        desc.type = RHITextureType::Texture2D;
        desc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
        desc.debugName = "AdapterRT";

        IRHITexture* texture = m_device->CreateTexture(desc);
        if (texture)
        {
            m_ownedTextures.push_back(texture);
        }
        return texture;
    }

    // ========================================================================
    // Resource creation -- samplers
    // ========================================================================

    IRHISampler* RHIAdapter::CreateSampler(const RHISamplerDesc& desc)
    {
        assert(m_device);

        IRHISampler* sampler = m_device->CreateSampler(desc);
        if (sampler)
        {
            m_ownedSamplers.push_back(sampler);
        }
        return sampler;
    }

    // ========================================================================
    // Resource creation -- pipeline states
    // ========================================================================

    IRHIPipelineState* RHIAdapter::CreateGraphicsPipeline(const RHIPipelineStateDesc& desc, IRHIShader* vertexShader,
                                                          IRHIShader* pixelShader)
    {
        assert(m_device);

        IRHIPipelineState* pso = m_device->CreatePipelineState(desc, vertexShader, pixelShader);
        if (pso)
        {
            m_ownedPipelineStates.push_back(pso);
        }
        return pso;
    }

    // ========================================================================
    // Resource creation -- shaders
    // ========================================================================

    IRHIShader* RHIAdapter::CreateShader(const RHIShaderDesc& desc)
    {
        assert(m_device);

        IRHIShader* shader = m_device->CreateShader(desc);
        if (shader)
        {
            m_ownedShaders.push_back(shader);
        }
        return shader;
    }

    // ========================================================================
    // Resource updates
    // ========================================================================

    void RHIAdapter::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
    {
        assert(m_device && buffer);
        m_device->UpdateBuffer(buffer, data, size, offset);
    }

    void* RHIAdapter::MapBuffer(IRHIBuffer* buffer)
    {
        assert(m_device && buffer);
        return m_device->MapBuffer(buffer);
    }

    void RHIAdapter::UnmapBuffer(IRHIBuffer* buffer)
    {
        assert(m_device && buffer);
        m_device->UnmapBuffer(buffer);
    }

    // ========================================================================
    // Resource destruction
    // ========================================================================

    namespace
    {
        /**
         * @brief Remove an element from a vector by value (unordered).
         */
        template <typename T> void RemoveFromVector(std::vector<T>& vec, T value)
        {
            auto it = std::find(vec.begin(), vec.end(), value);
            if (it != vec.end())
            {
                // Swap-and-pop for O(1) removal.
                std::iter_swap(it, vec.end() - 1);
                vec.pop_back();
            }
        }
    } // namespace

    void RHIAdapter::DestroyBuffer(IRHIBuffer* buffer)
    {
        if (!buffer || !m_device)
        {
            return;
        }
        RemoveFromVector(m_ownedBuffers, buffer);
        m_device->DestroyBuffer(buffer);
    }

    void RHIAdapter::DestroyTexture(IRHITexture* texture)
    {
        if (!texture || !m_device)
        {
            return;
        }
        RemoveFromVector(m_ownedTextures, texture);
        m_device->DestroyTexture(texture);
    }

    void RHIAdapter::DestroyShader(IRHIShader* shader)
    {
        if (!shader || !m_device)
        {
            return;
        }
        RemoveFromVector(m_ownedShaders, shader);
        m_device->DestroyShader(shader);
    }

    void RHIAdapter::DestroySampler(IRHISampler* sampler)
    {
        if (!sampler || !m_device)
        {
            return;
        }
        RemoveFromVector(m_ownedSamplers, sampler);
        m_device->DestroySampler(sampler);
    }

    void RHIAdapter::DestroyPipelineState(IRHIPipelineState* pso)
    {
        if (!pso || !m_device)
        {
            return;
        }
        RemoveFromVector(m_ownedPipelineStates, pso);
        m_device->DestroyPipelineState(pso);
    }

    // ========================================================================
    // Queries
    // ========================================================================

    const RHIDeviceCapabilities& RHIAdapter::GetCapabilities() const
    {
        assert(m_device);
        return m_device->GetCapabilities();
    }

    GraphicsBackend RHIAdapter::GetBackend() const
    {
        if (!m_device)
        {
            return GraphicsBackend::None;
        }
        return m_device->GetBackendType();
    }

    const RHIStatistics& RHIAdapter::GetStatistics() const
    {
        assert(m_device);
        return m_device->GetStatistics();
    }

} // namespace Spark::RHI
