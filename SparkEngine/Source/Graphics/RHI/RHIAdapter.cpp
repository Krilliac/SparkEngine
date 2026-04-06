/**
 * @file RHIAdapter.cpp
 * @brief Implementation of the RHI adapter layer (R3.2 -- Route rendering through RHI)
 * @author Spark Engine Team
 * @date 2026
 */

#include "RHIAdapter.h"
#include "../../Core/Contracts.h"
#include "../../Utils/Validate.h"

#include <algorithm>

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
        SPARK_EXPECTS(device != nullptr);
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

        // Release all tracked resources in reverse creation order.
        // unique_ptr destructors handle cleanup automatically.
        m_ownedPipelineStates.clear();
        m_ownedSamplers.clear();
        m_ownedShaders.clear();
        m_ownedTextures.clear();
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
        if (!m_commandList)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "RHIAdapter::BeginFrame -- GetImmediateCommandList returned null");
            return;
        }
        m_commandList->Begin();
        m_frameActive = true;
    }

    void RHIAdapter::EndFrame()
    {
        SPARK_REQUIRE_MSG(Spark::LogCategory::Graphics, m_frameActive, "RHIAdapter::EndFrame -- no frame in progress");

        if (!m_commandList)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "RHIAdapter::EndFrame -- command list is null");
            m_frameActive = false;
            return;
        }
        m_commandList->End();
        m_device->EndFrame();
        m_frameActive = false;
    }

    // ========================================================================
    // Pass markers
    // ========================================================================

    void RHIAdapter::BeginPass(const char* name)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->BeginEvent(name);
    }

    void RHIAdapter::EndPass()
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->EndEvent();
    }

    // ========================================================================
    // Render target binding
    // ========================================================================

    void RHIAdapter::SetRenderTargets(std::span<IRHITexture* const> renderTargets, IRHITexture* depthStencil)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);

        auto count = static_cast<uint32_t>(renderTargets.size());
        m_commandList->SetRenderTargets(renderTargets.data(), count, depthStencil);
    }

    void RHIAdapter::ClearRenderTarget(IRHITexture* target, const float color[4])
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->ClearRenderTarget(target, color);
    }

    void RHIAdapter::ClearDepthStencil(IRHITexture* target, float depth, uint8_t stencil)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->ClearDepthStencil(target, depth, stencil);
    }

    // ========================================================================
    // Viewport / scissor
    // ========================================================================

    void RHIAdapter::SetViewport(float x, float y, float width, float height, float minDepth, float maxDepth)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);

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
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);

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
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->SetPipelineState(pso);
    }

    void RHIAdapter::SetPrimitiveTopology(RHIPrimitiveTopology topology)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->SetPrimitiveTopology(topology);
    }

    // ========================================================================
    // Resource binding
    // ========================================================================

    void RHIAdapter::BindVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->SetVertexBuffer(buffer, slot, offset);
    }

    void RHIAdapter::BindIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->SetIndexBuffer(buffer, offset);
    }

    void RHIAdapter::BindConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->SetConstantBuffer(stage, slot, buffer);
    }

    void RHIAdapter::BindTexture(RHIShaderStage stage, uint32_t slot, IRHITexture* texture)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->SetShaderResource(stage, slot, texture);
    }

    void RHIAdapter::BindSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->SetSampler(stage, slot, sampler);
    }

    // ========================================================================
    // Draw commands
    // ========================================================================

    void RHIAdapter::Draw(uint32_t vertexCount, uint32_t startVertex)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->Draw(vertexCount, startVertex);
    }

    void RHIAdapter::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->DrawIndexed(indexCount, startIndex, baseVertex);
    }

    void RHIAdapter::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                   uint32_t startInstance)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->DrawInstanced(vertexCount, instanceCount, startVertex, startInstance);
    }

    void RHIAdapter::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount, uint32_t startIndex,
                                          int32_t baseVertex, uint32_t startInstance)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->DrawIndexedInstanced(indexCount, instanceCount, startIndex, baseVertex, startInstance);
    }

    // ========================================================================
    // Compute dispatch
    // ========================================================================

    void RHIAdapter::Dispatch(uint32_t groupsX, uint32_t groupsY, uint32_t groupsZ)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->Dispatch(groupsX, groupsY, groupsZ);
    }

    // ========================================================================
    // Indirect draw/dispatch
    // ========================================================================

    void RHIAdapter::DrawInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->DrawInstancedIndirect(argsBuffer, argsOffset);
    }

    void RHIAdapter::DrawIndexedInstancedIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->DrawIndexedInstancedIndirect(argsBuffer, argsOffset);
    }

    void RHIAdapter::DispatchIndirect(IRHIBuffer* argsBuffer, uint32_t argsOffset)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_commandList);
        m_commandList->DispatchIndirect(argsBuffer, argsOffset);
    }

    // ========================================================================
    // Resource creation -- buffers
    // ========================================================================

    IRHIBuffer* RHIAdapter::CreateVertexBuffer(const void* data, uint64_t size, uint32_t stride)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = stride;
        desc.usage = RHIBufferUsage::Vertex;
        desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
        desc.initialData = data;
        desc.debugName = "AdapterVB";

        auto buffer = m_device->CreateBuffer(desc);
        IRHIBuffer* raw = buffer.get();
        if (buffer)
            m_ownedBuffers.push_back(std::move(buffer));
        return raw;
    }

    IRHIBuffer* RHIAdapter::CreateIndexBuffer(const void* data, uint64_t size, uint32_t stride)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = stride;
        desc.usage = RHIBufferUsage::Index;
        desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
        desc.initialData = data;
        desc.debugName = "AdapterIB";

        auto buffer = m_device->CreateBuffer(desc);
        IRHIBuffer* raw = buffer.get();
        if (buffer)
            m_ownedBuffers.push_back(std::move(buffer));
        return raw;
    }

    IRHIBuffer* RHIAdapter::CreateConstantBuffer(uint64_t size)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = 0;
        desc.usage = RHIBufferUsage::Constant;
        desc.access = RHIBufferAccess::Dynamic;
        desc.initialData = nullptr;
        desc.debugName = "AdapterCB";

        auto buffer = m_device->CreateBuffer(desc);
        IRHIBuffer* raw = buffer.get();
        if (buffer)
            m_ownedBuffers.push_back(std::move(buffer));
        return raw;
    }

    IRHIBuffer* RHIAdapter::CreateStructuredBuffer(const void* data, uint64_t size, uint32_t stride)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

        RHIBufferDesc desc{};
        desc.size = size;
        desc.stride = stride;
        desc.usage = RHIBufferUsage::Structured | RHIBufferUsage::Storage;
        desc.access = data ? RHIBufferAccess::Static : RHIBufferAccess::Dynamic;
        desc.initialData = data;
        desc.debugName = "AdapterSB";

        auto buffer = m_device->CreateBuffer(desc);
        IRHIBuffer* raw = buffer.get();
        if (buffer)
            m_ownedBuffers.push_back(std::move(buffer));
        return raw;
    }

    // ========================================================================
    // Resource creation -- textures
    // ========================================================================

    IRHITexture* RHIAdapter::CreateTexture2D(uint32_t width, uint32_t height, PixelFormat format, RHITextureUsage usage,
                                             const void* initialData)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

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

        auto texture = m_device->CreateTexture(desc);
        IRHITexture* raw = texture.get();
        if (texture)
        {
            if (initialData)
                m_device->UpdateTexture(raw, initialData);
            m_ownedTextures.push_back(std::move(texture));
        }
        return raw;
    }

    IRHITexture* RHIAdapter::CreateDepthStencil(uint32_t width, uint32_t height, PixelFormat format)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

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

        auto texture = m_device->CreateTexture(desc);
        IRHITexture* raw = texture.get();
        if (texture)
            m_ownedTextures.push_back(std::move(texture));
        return raw;
    }

    IRHITexture* RHIAdapter::CreateRenderTarget(uint32_t width, uint32_t height, PixelFormat format)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

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

        auto texture = m_device->CreateTexture(desc);
        IRHITexture* raw = texture.get();
        if (texture)
            m_ownedTextures.push_back(std::move(texture));
        return raw;
    }

    // ========================================================================
    // Resource creation -- samplers
    // ========================================================================

    IRHISampler* RHIAdapter::CreateSampler(const RHISamplerDesc& desc)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

        auto sampler = m_device->CreateSampler(desc);
        IRHISampler* raw = sampler.get();
        if (sampler)
            m_ownedSamplers.push_back(std::move(sampler));
        return raw;
    }

    // ========================================================================
    // Resource creation -- pipeline states
    // ========================================================================

    IRHIPipelineState* RHIAdapter::CreateGraphicsPipeline(const RHIPipelineStateDesc& desc, IRHIShader* vertexShader,
                                                          IRHIShader* pixelShader)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

        auto pso = m_device->CreatePipelineState(desc, vertexShader, pixelShader);
        IRHIPipelineState* raw = pso.get();
        if (pso)
            m_ownedPipelineStates.push_back(std::move(pso));
        return raw;
    }

    // ========================================================================
    // Resource creation -- shaders
    // ========================================================================

    IRHIShader* RHIAdapter::CreateShader(const RHIShaderDesc& desc)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);

        auto shader = m_device->CreateShader(desc);
        IRHIShader* raw = shader.get();
        if (shader)
            m_ownedShaders.push_back(std::move(shader));
        return raw;
    }

    // ========================================================================
    // Resource updates
    // ========================================================================

    void RHIAdapter::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_device);
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, buffer);
        m_device->UpdateBuffer(buffer, data, size, offset);
    }

    void* RHIAdapter::MapBuffer(IRHIBuffer* buffer)
    {
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, m_device, nullptr);
        SPARK_VALIDATE_NOT_NULL_RET(Spark::LogCategory::Graphics, buffer, nullptr);
        return m_device->MapBuffer(buffer);
    }

    void RHIAdapter::UnmapBuffer(IRHIBuffer* buffer)
    {
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, m_device);
        SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Graphics, buffer);
        m_device->UnmapBuffer(buffer);
    }

    // ========================================================================
    // Resource destruction
    // ========================================================================

    namespace
    {
        /**
         * @brief Remove a unique_ptr element from a vector by raw pointer (unordered).
         */
        template <typename T> void RemoveFromVector(std::vector<std::unique_ptr<T>>& vec, T* value)
        {
            auto it = std::find_if(vec.begin(), vec.end(), [value](const auto& p) { return p.get() == value; });
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
        if (!buffer)
            return;
        RemoveFromVector(m_ownedBuffers, buffer);
    }

    void RHIAdapter::DestroyTexture(IRHITexture* texture)
    {
        if (!texture)
            return;
        RemoveFromVector(m_ownedTextures, texture);
    }

    void RHIAdapter::DestroyShader(IRHIShader* shader)
    {
        if (!shader)
            return;
        RemoveFromVector(m_ownedShaders, shader);
    }

    void RHIAdapter::DestroySampler(IRHISampler* sampler)
    {
        if (!sampler)
            return;
        RemoveFromVector(m_ownedSamplers, sampler);
    }

    void RHIAdapter::DestroyPipelineState(IRHIPipelineState* pso)
    {
        if (!pso)
            return;
        RemoveFromVector(m_ownedPipelineStates, pso);
    }

    // ========================================================================
    // Queries
    // ========================================================================

    const RHIDeviceCapabilities& RHIAdapter::GetCapabilities() const
    {
        SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, m_device);
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
        SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Graphics, m_device);
        return m_device->GetStatistics();
    }

} // namespace Spark::RHI
