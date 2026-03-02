/**
 * @file RHIResources.h
 * @brief Abstract resource interfaces for the Rendering Hardware Interface
 * @author Spark Engine Team
 * @date 2025
 *
 * Defines the abstract base classes for all GPU resources managed by
 * the RHI backends (buffers, textures, shaders, samplers, pipeline states).
 */

#pragma once

#include "RHITypes.h"

namespace Spark { namespace RHI {

/**
 * @brief Base class for all RHI resources
 */
class IRHIResource
{
public:
    virtual ~IRHIResource() = default;
    virtual const std::string& GetDebugName() const = 0;
    virtual void SetDebugName(const std::string& name) = 0;
    virtual bool IsValid() const = 0;
};

/**
 * @brief Abstract GPU buffer resource
 */
class IRHIBuffer : public IRHIResource
{
public:
    virtual ~IRHIBuffer() = default;

    virtual const RHIBufferDesc& GetDesc() const = 0;
    virtual uint64_t GetSize() const = 0;
    virtual uint32_t GetStride() const = 0;
    virtual void* GetNativeHandle() const = 0;
};

/**
 * @brief Abstract GPU texture resource
 */
class IRHITexture : public IRHIResource
{
public:
    virtual ~IRHITexture() = default;

    virtual const RHITextureDesc& GetDesc() const = 0;
    virtual uint32_t GetWidth() const = 0;
    virtual uint32_t GetHeight() const = 0;
    virtual uint32_t GetDepth() const = 0;
    virtual uint32_t GetMipLevels() const = 0;
    virtual PixelFormat GetFormat() const = 0;
    virtual void* GetNativeHandle() const = 0;
    virtual void* GetShaderResourceView() const = 0;
    virtual void* GetRenderTargetView() const = 0;
    virtual void* GetDepthStencilView() const = 0;
};

/**
 * @brief Abstract GPU shader resource
 */
class IRHIShader : public IRHIResource
{
public:
    virtual ~IRHIShader() = default;

    virtual RHIShaderStage GetStage() const = 0;
    virtual const std::string& GetEntryPoint() const = 0;
    virtual void* GetNativeHandle() const = 0;
    virtual const void* GetBytecode() const = 0;
    virtual size_t GetBytecodeSize() const = 0;
};

/**
 * @brief Abstract GPU sampler state
 */
class IRHISampler : public IRHIResource
{
public:
    virtual ~IRHISampler() = default;

    virtual const RHISamplerDesc& GetDesc() const = 0;
    virtual void* GetNativeHandle() const = 0;
};

/**
 * @brief Abstract graphics pipeline state
 */
class IRHIPipelineState : public IRHIResource
{
public:
    virtual ~IRHIPipelineState() = default;

    virtual const RHIPipelineStateDesc& GetDesc() const = 0;
    virtual void* GetNativeHandle() const = 0;
};

}} // namespace Spark::RHI
