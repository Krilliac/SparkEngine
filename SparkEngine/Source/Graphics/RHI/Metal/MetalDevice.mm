/**
 * @file MetalDevice.mm
 * @brief Apple Metal implementation of Spark RHI
 */

#ifdef __APPLE__

#include "MetalDevice.h"

#include "../../../Utils/Logger.h"

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace Spark
{
    namespace RHI
    {
        namespace Metal
        {
            namespace
            {
                static NSString* ToNSString(const std::string& value)
                {
                    return [NSString stringWithUTF8String:value.c_str()];
                }
            }

            MetalBuffer::MetalBuffer(const RHIBufferDesc& desc, id<MTLBuffer> buffer)
                : m_desc(desc), m_buffer(buffer)
            {
            }

            MetalTexture::MetalTexture(const RHITextureDesc& desc, id<MTLTexture> texture)
                : m_desc(desc), m_texture(texture)
            {
            }

            MetalShader::MetalShader(const RHIShaderDesc& desc, id<MTLLibrary> library, id<MTLFunction> function)
                : m_desc(desc), m_library(library), m_function(function)
            {
            }

            MetalSampler::MetalSampler(const RHISamplerDesc& desc, id<MTLSamplerState> sampler)
                : m_desc(desc), m_sampler(sampler)
            {
            }

            MetalPipelineState::MetalPipelineState(const RHIPipelineStateDesc& desc,
                                                   id<MTLRenderPipelineState> renderPipeline,
                                                   id<MTLDepthStencilState> depthStencilState,
                                                   MetalShader* vertexShader,
                                                   MetalShader* fragmentShader)
                : m_desc(desc),
                  m_renderPipeline(renderPipeline),
                  m_depthStencilState(depthStencilState),
                  m_vertexShader(vertexShader),
                  m_fragmentShader(fragmentShader)
            {
            }

            MetalComputePipelineState::MetalComputePipelineState(id<MTLComputePipelineState> pipeline,
                                                                 const std::string& debugName)
                : m_pipeline(pipeline), m_debugName(debugName)
            {
            }

            NSUInteger MetalComputePipelineState::GetThreadExecutionWidth() const
            {
                return m_pipeline ? m_pipeline.Get().threadExecutionWidth : 0;
            }

            NSUInteger MetalComputePipelineState::GetMaxTotalThreadsPerThreadgroup() const
            {
                return m_pipeline ? m_pipeline.Get().maxTotalThreadsPerThreadgroup : 0;
            }

            MetalArgumentBuffer::MetalArgumentBuffer(id<MTLBuffer> argumentBuffer,
                                                     id<MTLArgumentEncoder> encoder,
                                                     uint32_t maxEntries)
                : m_argumentBuffer(argumentBuffer), m_encoder(encoder), m_maxEntries(maxEntries)
            {
            }

            void MetalArgumentBuffer::SetBuffer(uint32_t index, id<MTLBuffer> buffer, uint64_t offset)
            {
                if (!m_encoder || index >= m_maxEntries)
                {
                    return;
                }
                [m_encoder.Get() setArgumentBuffer:m_argumentBuffer.Get() offset:0];
                [m_encoder.Get() setBuffer:buffer offset:offset atIndex:index];
            }

            void MetalArgumentBuffer::SetTexture(uint32_t index, id<MTLTexture> texture)
            {
                if (!m_encoder || index >= m_maxEntries)
                {
                    return;
                }
                [m_encoder.Get() setArgumentBuffer:m_argumentBuffer.Get() offset:0];
                [m_encoder.Get() setTexture:texture atIndex:index];
            }

            void MetalArgumentBuffer::SetSampler(uint32_t index, id<MTLSamplerState> sampler)
            {
                if (!m_encoder || index >= m_maxEntries)
                {
                    return;
                }
                [m_encoder.Get() setArgumentBuffer:m_argumentBuffer.Get() offset:0];
                [m_encoder.Get() setSamplerState:sampler atIndex:index];
            }

            MetalSwapChain::MetalSwapChain(id<MTLDevice> device, const RHISwapChainDesc& desc)
                : m_desc(desc), m_device(device), m_metalLayer(nil), m_currentDrawable(nil)
            {
                ConfigureMetalLayer();
            }

            MetalSwapChain::~MetalSwapChain()
            {
                m_currentDrawable = nil;
                m_backBuffer.reset();
            }

            bool MetalSwapChain::ConfigureMetalLayer()
            {
                if (m_desc.windowHandle == nullptr)
                {
                    return false;
                }

                NSView* view = (__bridge NSView*)m_desc.windowHandle;
                if (view == nil)
                {
                    return false;
                }

                [view setWantsLayer:YES];
                CAMetalLayer* layer = [CAMetalLayer layer];
                layer.device = m_device;
                layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
                layer.framebufferOnly = NO;
                layer.drawableSize = CGSizeMake(static_cast<CGFloat>(m_desc.width), static_cast<CGFloat>(m_desc.height));

                view.layer = layer;
                m_metalLayer = layer;
                return true;
            }

            void MetalSwapChain::AcquireNextDrawable()
            {
                if (m_metalLayer == nil)
                {
                    return;
                }

                m_currentDrawable = [m_metalLayer nextDrawable];
                if (m_currentDrawable == nil)
                {
                    return;
                }

                if (!m_backBuffer)
                {
                    RHITextureDesc backBufferDesc{};
                    backBufferDesc.width = m_desc.width;
                    backBufferDesc.height = m_desc.height;
                    backBufferDesc.format = m_desc.format;
                    backBufferDesc.usage = RHITextureUsage::RenderTarget | RHITextureUsage::ShaderResource;
                    backBufferDesc.debugName = "MetalBackBuffer";
                    m_backBuffer = std::make_unique<MetalTexture>(backBufferDesc, m_currentDrawable.texture);
                }
                else
                {
                    m_backBuffer->SetMTLTexture(m_currentDrawable.texture);
                }
            }

            bool MetalSwapChain::Present(bool /*vsync*/)
            {
                // Presentation is driven by the command buffer in ExecuteCommandList.
                m_currentBufferIndex = (m_currentBufferIndex + 1u) % std::max(1u, m_desc.bufferCount);
                return true;
            }

            bool MetalSwapChain::Resize(uint32_t width, uint32_t height)
            {
                m_desc.width = width;
                m_desc.height = height;

                if (m_metalLayer != nil)
                {
                    m_metalLayer.drawableSize = CGSizeMake(static_cast<CGFloat>(width), static_cast<CGFloat>(height));
                }

                m_backBuffer.reset();
                return true;
            }

            IRHITexture* MetalSwapChain::GetBackBuffer()
            {
                AcquireNextDrawable();
                return m_backBuffer.get();
            }

            MetalCommandList::MetalCommandList(id<MTLCommandQueue> commandQueue, bool isImmediate)
                : m_commandQueue(commandQueue), m_isImmediate(isImmediate)
            {
            }

            void MetalCommandList::Begin()
            {
                if (!m_commandQueue)
                {
                    return;
                }

                id<MTLCommandBuffer> commandBuffer = [m_commandQueue.Get() commandBuffer];
                m_commandBuffer.Reset(commandBuffer);
                m_renderEncoder.Reset(nil);
                m_computeEncoder.Reset(nil);
                m_currentRenderPassDesc = nil;
            }

            void MetalCommandList::EndCurrentEncoder()
            {
                if (m_renderEncoder)
                {
                    [m_renderEncoder.Get() endEncoding];
                    m_renderEncoder.Reset(nil);
                }
                if (m_computeEncoder)
                {
                    [m_computeEncoder.Get() endEncoding];
                    m_computeEncoder.Reset(nil);
                }
            }

            void MetalCommandList::End()
            {
                EndCurrentEncoder();
            }

            void MetalCommandList::Reset()
            {
                EndCurrentEncoder();
                m_commandBuffer.Reset(nil);
                m_currentRenderPassDesc = nil;
            }

            void MetalCommandList::SetRenderTargets(IRHITexture* const* renderTargets, uint32_t count, IRHITexture* /*depthStencil*/)
            {
                if (count == 0 || renderTargets == nullptr || renderTargets[0] == nullptr)
                {
                    return;
                }

                auto* rt = dynamic_cast<MetalTexture*>(renderTargets[0]);
                if (rt == nullptr)
                {
                    return;
                }

                MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
                passDesc.colorAttachments[0].texture = rt->GetMTLTexture();
                passDesc.colorAttachments[0].loadAction = MTLLoadActionLoad;
                passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;

                m_currentRenderPassDesc = passDesc;
                m_renderEncoder.Reset(nil);
            }

            void MetalCommandList::EnsureRenderEncoder()
            {
                if (m_renderEncoder || !m_commandBuffer || m_currentRenderPassDesc == nil)
                {
                    return;
                }

                id<MTLRenderCommandEncoder> encoder = [m_commandBuffer.Get() renderCommandEncoderWithDescriptor:m_currentRenderPassDesc];
                m_renderEncoder.Reset(encoder);
            }

            void MetalCommandList::EnsureComputeEncoder()
            {
                if (m_computeEncoder || !m_commandBuffer)
                {
                    return;
                }
                id<MTLComputeCommandEncoder> encoder = [m_commandBuffer.Get() computeCommandEncoder];
                m_computeEncoder.Reset(encoder);
            }

            void MetalCommandList::ClearRenderTarget(IRHITexture* target, const float color[4])
            {
                auto* rt = dynamic_cast<MetalTexture*>(target);
                if (rt == nullptr || !m_commandBuffer)
                {
                    return;
                }

                MTLRenderPassDescriptor* passDesc = [MTLRenderPassDescriptor renderPassDescriptor];
                passDesc.colorAttachments[0].texture = rt->GetMTLTexture();
                passDesc.colorAttachments[0].loadAction = MTLLoadActionClear;
                passDesc.colorAttachments[0].storeAction = MTLStoreActionStore;
                passDesc.colorAttachments[0].clearColor = MTLClearColorMake(color[0], color[1], color[2], color[3]);

                id<MTLRenderCommandEncoder> encoder = [m_commandBuffer.Get() renderCommandEncoderWithDescriptor:passDesc];
                [encoder endEncoding];
            }

            void MetalCommandList::ClearDepthStencil(IRHITexture* /*target*/, float /*depth*/, uint8_t /*stencil*/)
            {
            }

            void MetalCommandList::SetViewport(const RHIViewport& viewport)
            {
                EnsureRenderEncoder();
                if (!m_renderEncoder)
                {
                    return;
                }

                MTLViewport metalViewport;
                metalViewport.originX = viewport.x;
                metalViewport.originY = viewport.y;
                metalViewport.width = viewport.width;
                metalViewport.height = viewport.height;
                metalViewport.znear = viewport.minDepth;
                metalViewport.zfar = viewport.maxDepth;
                [m_renderEncoder.Get() setViewport:metalViewport];
            }

            void MetalCommandList::SetScissorRect(const RHIScissorRect& rect)
            {
                EnsureRenderEncoder();
                if (!m_renderEncoder)
                {
                    return;
                }

                MTLScissorRect scissorRect;
                scissorRect.x = static_cast<NSUInteger>(std::max(0, rect.left));
                scissorRect.y = static_cast<NSUInteger>(std::max(0, rect.top));
                scissorRect.width = static_cast<NSUInteger>(std::max(0, rect.right - rect.left));
                scissorRect.height = static_cast<NSUInteger>(std::max(0, rect.bottom - rect.top));
                [m_renderEncoder.Get() setScissorRect:scissorRect];
            }

            void MetalCommandList::SetPipelineState(IRHIPipelineState* pipelineState)
            {
                auto* state = dynamic_cast<MetalPipelineState*>(pipelineState);
                EnsureRenderEncoder();
                if (!m_renderEncoder || state == nullptr)
                {
                    return;
                }

                [m_renderEncoder.Get() setRenderPipelineState:state->GetMTLRenderPipelineState()];
                if (state->GetMTLDepthStencilState() != nil)
                {
                    [m_renderEncoder.Get() setDepthStencilState:state->GetMTLDepthStencilState()];
                }
            }

            MTLPrimitiveType MetalCommandList::ConvertTopology(RHIPrimitiveTopology topology) const
            {
                switch (topology)
                {
                case RHIPrimitiveTopology::PointList:
                    return MTLPrimitiveTypePoint;
                case RHIPrimitiveTopology::LineList:
                case RHIPrimitiveTopology::LineStrip:
                    return MTLPrimitiveTypeLine;
                case RHIPrimitiveTopology::TriangleStrip:
                    return MTLPrimitiveTypeTriangleStrip;
                case RHIPrimitiveTopology::PatchList:
                case RHIPrimitiveTopology::TriangleList:
                default:
                    return MTLPrimitiveTypeTriangle;
                }
            }

            void MetalCommandList::SetPrimitiveTopology(RHIPrimitiveTopology topology)
            {
                m_currentTopology = ConvertTopology(topology);
            }

            void MetalCommandList::SetVertexBuffer(IRHIBuffer* buffer, uint32_t slot, uint32_t offset)
            {
                auto* metalBuffer = dynamic_cast<MetalBuffer*>(buffer);
                EnsureRenderEncoder();
                if (!m_renderEncoder || metalBuffer == nullptr)
                {
                    return;
                }
                [m_renderEncoder.Get() setVertexBuffer:metalBuffer->GetMTLBuffer() offset:offset atIndex:slot];
            }

            void MetalCommandList::SetIndexBuffer(IRHIBuffer* buffer, uint32_t offset)
            {
                auto* metalBuffer = dynamic_cast<MetalBuffer*>(buffer);
                if (metalBuffer == nullptr)
                {
                    return;
                }
                m_currentIndexBuffer.Reset(metalBuffer->GetMTLBuffer());
                m_currentIndexBufferOffset = offset;
            }

            void MetalCommandList::SetConstantBuffer(RHIShaderStage stage, uint32_t slot, IRHIBuffer* buffer)
            {
                auto* metalBuffer = dynamic_cast<MetalBuffer*>(buffer);
                if (metalBuffer == nullptr)
                {
                    return;
                }

                EnsureRenderEncoder();
                if (!m_renderEncoder)
                {
                    return;
                }

                if (stage == RHIShaderStage::Vertex)
                {
                    [m_renderEncoder.Get() setVertexBuffer:metalBuffer->GetMTLBuffer() offset:0 atIndex:slot];
                }
                else
                {
                    [m_renderEncoder.Get() setFragmentBuffer:metalBuffer->GetMTLBuffer() offset:0 atIndex:slot];
                }
            }

            void MetalCommandList::SetShaderResource(RHIShaderStage stage, uint32_t slot, IRHITexture* texture)
            {
                auto* metalTexture = dynamic_cast<MetalTexture*>(texture);
                EnsureRenderEncoder();
                if (!m_renderEncoder || metalTexture == nullptr)
                {
                    return;
                }

                if (stage == RHIShaderStage::Vertex)
                {
                    [m_renderEncoder.Get() setVertexTexture:metalTexture->GetMTLTexture() atIndex:slot];
                }
                else
                {
                    [m_renderEncoder.Get() setFragmentTexture:metalTexture->GetMTLTexture() atIndex:slot];
                }
            }

            void MetalCommandList::SetSampler(RHIShaderStage stage, uint32_t slot, IRHISampler* sampler)
            {
                auto* metalSampler = dynamic_cast<MetalSampler*>(sampler);
                EnsureRenderEncoder();
                if (!m_renderEncoder || metalSampler == nullptr)
                {
                    return;
                }

                if (stage == RHIShaderStage::Vertex)
                {
                    [m_renderEncoder.Get() setVertexSamplerState:metalSampler->GetMTLSampler() atIndex:slot];
                }
                else
                {
                    [m_renderEncoder.Get() setFragmentSamplerState:metalSampler->GetMTLSampler() atIndex:slot];
                }
            }

            void MetalCommandList::Draw(uint32_t vertexCount, uint32_t startVertex)
            {
                EnsureRenderEncoder();
                if (!m_renderEncoder)
                {
                    return;
                }
                [m_renderEncoder.Get() drawPrimitives:m_currentTopology vertexStart:startVertex vertexCount:vertexCount];
            }

            void MetalCommandList::DrawIndexed(uint32_t indexCount, uint32_t startIndex, int32_t baseVertex)
            {
                EnsureRenderEncoder();
                if (!m_renderEncoder || !m_currentIndexBuffer)
                {
                    return;
                }

                const NSUInteger indexOffset = m_currentIndexBufferOffset + startIndex * sizeof(uint32_t);
                [m_renderEncoder.Get() drawIndexedPrimitives:m_currentTopology
                                                  indexCount:indexCount
                                                   indexType:MTLIndexTypeUInt32
                                                 indexBuffer:m_currentIndexBuffer.Get()
                                           indexBufferOffset:indexOffset
                                               instanceCount:1
                                                  baseVertex:baseVertex
                                                baseInstance:0];
            }

            void MetalCommandList::DrawInstanced(uint32_t vertexCount, uint32_t instanceCount, uint32_t startVertex,
                                                 uint32_t startInstance)
            {
                EnsureRenderEncoder();
                if (!m_renderEncoder)
                {
                    return;
                }

                [m_renderEncoder.Get() drawPrimitives:m_currentTopology
                                          vertexStart:startVertex
                                          vertexCount:vertexCount
                                        instanceCount:instanceCount
                                         baseInstance:startInstance];
            }

            void MetalCommandList::DrawIndexedInstanced(uint32_t indexCount, uint32_t instanceCount,
                                                        uint32_t startIndex, int32_t baseVertex,
                                                        uint32_t startInstance)
            {
                EnsureRenderEncoder();
                if (!m_renderEncoder || !m_currentIndexBuffer)
                {
                    return;
                }

                const NSUInteger indexOffset = m_currentIndexBufferOffset + startIndex * sizeof(uint32_t);
                [m_renderEncoder.Get() drawIndexedPrimitives:m_currentTopology
                                                  indexCount:indexCount
                                                   indexType:MTLIndexTypeUInt32
                                                 indexBuffer:m_currentIndexBuffer.Get()
                                           indexBufferOffset:indexOffset
                                               instanceCount:instanceCount
                                                  baseVertex:baseVertex
                                                baseInstance:startInstance];
            }

            void MetalCommandList::Dispatch(uint32_t x, uint32_t y, uint32_t z)
            {
                EnsureComputeEncoder();
                if (!m_computeEncoder)
                {
                    return;
                }

                MTLSize grid = MTLSizeMake(x, y, z);
                MTLSize group = MTLSizeMake(8, 8, 1);
                [m_computeEncoder.Get() dispatchThreads:grid threadsPerThreadgroup:group];
            }

            void MetalCommandList::DrawInstancedIndirect(IRHIBuffer* /*argsBuffer*/, uint32_t /*argsOffset*/)
            {
            }

            void MetalCommandList::DrawIndexedInstancedIndirect(IRHIBuffer* /*argsBuffer*/, uint32_t /*argsOffset*/)
            {
            }

            void MetalCommandList::DispatchIndirect(IRHIBuffer* /*argsBuffer*/, uint32_t /*argsOffset*/)
            {
            }

            void MetalCommandList::CopyTexture(IRHITexture* dst, IRHITexture* src)
            {
                auto* dstTexture = dynamic_cast<MetalTexture*>(dst);
                auto* srcTexture = dynamic_cast<MetalTexture*>(src);
                if (dstTexture == nullptr || srcTexture == nullptr || !m_commandBuffer)
                {
                    return;
                }

                id<MTLBlitCommandEncoder> blitEncoder = [m_commandBuffer.Get() blitCommandEncoder];
                [blitEncoder copyFromTexture:srcTexture->GetMTLTexture()
                                sourceSlice:0
                                sourceLevel:0
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:MTLSizeMake(srcTexture->GetWidth(), srcTexture->GetHeight(), 1)
                                  toTexture:dstTexture->GetMTLTexture()
                           destinationSlice:0
                           destinationLevel:0
                          destinationOrigin:MTLOriginMake(0, 0, 0)];
                [blitEncoder endEncoding];
            }

            void MetalCommandList::BeginEvent(const char* name)
            {
                if (m_commandBuffer && name != nullptr)
                {
                    m_commandBuffer.Get().label = ToNSString(name);
                }
            }

            void MetalCommandList::EndEvent()
            {
            }

            void MetalCommandList::SetMarker(const char* name)
            {
                if (m_renderEncoder && name != nullptr)
                {
                    [m_renderEncoder.Get() insertDebugSignpost:ToNSString(name)];
                }
            }

            void MetalCommandList::SetComputePipelineState(MetalComputePipelineState* computePipeline)
            {
                EnsureComputeEncoder();
                if (!m_computeEncoder || computePipeline == nullptr || !computePipeline->IsValid())
                {
                    return;
                }

                [m_computeEncoder.Get() setComputePipelineState:computePipeline->GetMTLComputePipelineState()];
            }

            void MetalCommandList::SetArgumentBuffer(RHIShaderStage stage, uint32_t slot, MetalArgumentBuffer* argumentBuffer)
            {
                if (argumentBuffer == nullptr)
                {
                    return;
                }

                EnsureRenderEncoder();
                if (!m_renderEncoder)
                {
                    return;
                }

                if (stage == RHIShaderStage::Vertex)
                {
                    [m_renderEncoder.Get() setVertexBuffer:argumentBuffer->GetMTLBuffer() offset:0 atIndex:slot];
                }
                else
                {
                    [m_renderEncoder.Get() setFragmentBuffer:argumentBuffer->GetMTLBuffer() offset:0 atIndex:slot];
                }
            }

            MetalDevice::MetalDevice() = default;

            MetalDevice::~MetalDevice()
            {
                Shutdown();
            }

            bool MetalDevice::Initialize(const RHIDeviceDesc& desc)
            {
                m_debugEnabled = desc.enableDebugLayer;

                id<MTLDevice> device = MTLCreateSystemDefaultDevice();
                if (device == nil)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Metal: failed to create default MTLDevice");
                    return false;
                }

                m_device.Reset(device);

                id<MTLCommandQueue> queue = [m_device.Get() newCommandQueue];
                if (queue == nil)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Metal: failed to create command queue");
                    return false;
                }
                m_commandQueue.Reset(queue);

                m_immediateCommandList = std::make_unique<MetalCommandList>(m_commandQueue.Get(), true);

                DetectFeatures();
                PopulateCapabilities();
                ResetStatistics();

                return true;
            }

            void MetalDevice::Shutdown()
            {
                WaitForIdle();
                m_immediateCommandList.reset();
                m_commandQueue.Reset(nil);
                m_device.Reset(nil);
            }

            std::unique_ptr<IRHISwapChain> MetalDevice::CreateSwapChain(const RHISwapChainDesc& desc)
            {
                return std::make_unique<MetalSwapChain>(m_device.Get(), desc);
            }

            std::unique_ptr<IRHIBuffer> MetalDevice::CreateBuffer(const RHIBufferDesc& desc)
            {
                MTLResourceOptions options = ConvertBufferAccess(desc.access);
                id<MTLBuffer> buffer = [m_device.Get() newBufferWithLength:desc.size options:options];
                if (buffer == nil)
                {
                    return nullptr;
                }

                if (desc.initialData != nullptr && desc.size > 0)
                {
                    std::memcpy([buffer contents], desc.initialData, static_cast<size_t>(desc.size));
                }

                if (!desc.debugName.empty())
                {
                    buffer.label = ToNSString(desc.debugName);
                }

                return std::make_unique<MetalBuffer>(desc, buffer);
            }

            std::unique_ptr<IRHITexture> MetalDevice::CreateTexture(const RHITextureDesc& desc)
            {
                MTLTextureDescriptor* textureDesc = [[MTLTextureDescriptor alloc] init];
                textureDesc.textureType = ConvertTextureType(desc.type);
                textureDesc.width = desc.width;
                textureDesc.height = desc.height;
                textureDesc.depth = desc.depth;
                textureDesc.mipmapLevelCount = desc.mipLevels;
                textureDesc.arrayLength = desc.arraySize;
                textureDesc.sampleCount = desc.sampleCount;
                textureDesc.pixelFormat = ConvertFormat(desc.format);
                textureDesc.storageMode = MTLStorageModePrivate;
                textureDesc.usage = MTLTextureUsageUnknown;

                if (desc.usage & RHITextureUsage::ShaderResource)
                {
                    textureDesc.usage |= MTLTextureUsageShaderRead;
                }
                if (desc.usage & RHITextureUsage::RenderTarget)
                {
                    textureDesc.usage |= MTLTextureUsageRenderTarget;
                }
                if (desc.usage & RHITextureUsage::UnorderedAccess)
                {
                    textureDesc.usage |= MTLTextureUsageShaderWrite;
                }

                id<MTLTexture> texture = [m_device.Get() newTextureWithDescriptor:textureDesc];
                if (texture == nil)
                {
                    return nullptr;
                }

                if (!desc.debugName.empty())
                {
                    texture.label = ToNSString(desc.debugName);
                }

                return std::make_unique<MetalTexture>(desc, texture);
            }

            std::unique_ptr<IRHITexture> MetalDevice::WrapNativeTexture(void* nativeHandle,
                                                                         const RHITextureDesc& desc)
            {
                id<MTLTexture> texture = (__bridge id<MTLTexture>)nativeHandle;
                if (texture == nil)
                {
                    return nullptr;
                }
                return std::make_unique<MetalTexture>(desc, texture);
            }

            std::unique_ptr<IRHIShader> MetalDevice::CreateShader(const RHIShaderDesc& desc)
            {
                NSError* error = nil;
                id<MTLLibrary> library = nil;

                if (!desc.sourceCode.empty())
                {
                    NSString* source = ToNSString(desc.sourceCode);
                    library = [m_device.Get() newLibraryWithSource:source options:nil error:&error];
                }
                else if (desc.bytecode != nullptr && desc.bytecodeSize > 0)
                {
                    dispatch_data_t data = dispatch_data_create(desc.bytecode, desc.bytecodeSize,
                                                                dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0),
                                                                DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                    library = [m_device.Get() newLibraryWithData:data error:&error];
                }

                if (library == nil)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Metal shader compile failed: %s",
                                    error != nil ? error.localizedDescription.UTF8String : "unknown error");
                    return nullptr;
                }

                NSString* entryPoint = ToNSString(desc.entryPoint);
                id<MTLFunction> function = [library newFunctionWithName:entryPoint];
                if (function == nil)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Metal shader entry point not found: %s",
                                    desc.entryPoint.c_str());
                    return nullptr;
                }

                if (!desc.debugName.empty())
                {
                    function.label = ToNSString(desc.debugName);
                }

                return std::make_unique<MetalShader>(desc, library, function);
            }

            std::unique_ptr<IRHISampler> MetalDevice::CreateSampler(const RHISamplerDesc& desc)
            {
                MTLSamplerDescriptor* samplerDesc = [[MTLSamplerDescriptor alloc] init];
                samplerDesc.minFilter = ConvertMinMagFilter(desc.minFilter);
                samplerDesc.magFilter = ConvertMinMagFilter(desc.magFilter);
                samplerDesc.mipFilter = ConvertMipFilter(desc.mipFilter);
                samplerDesc.sAddressMode = ConvertAddressMode(desc.addressU);
                samplerDesc.tAddressMode = ConvertAddressMode(desc.addressV);
                samplerDesc.rAddressMode = ConvertAddressMode(desc.addressW);
                samplerDesc.lodMinClamp = desc.minLod;
                samplerDesc.lodMaxClamp = desc.maxLod;
                samplerDesc.maxAnisotropy = desc.maxAnisotropy;
                samplerDesc.compareFunction = ConvertCompareOp(desc.compareOp);

                id<MTLSamplerState> sampler = [m_device.Get() newSamplerStateWithDescriptor:samplerDesc];
                if (sampler == nil)
                {
                    return nullptr;
                }

                return std::make_unique<MetalSampler>(desc, sampler);
            }

            std::unique_ptr<IRHIPipelineState> MetalDevice::CreatePipelineState(const RHIPipelineStateDesc& desc,
                                                                                 IRHIShader* vertexShader,
                                                                                 IRHIShader* pixelShader)
            {
                auto* metalVS = dynamic_cast<MetalShader*>(vertexShader);
                auto* metalPS = dynamic_cast<MetalShader*>(pixelShader);
                if (metalVS == nullptr || metalPS == nullptr)
                {
                    return nullptr;
                }

                MTLRenderPipelineDescriptor* psoDesc = [[MTLRenderPipelineDescriptor alloc] init];
                psoDesc.vertexFunction = metalVS->GetMTLFunction();
                psoDesc.fragmentFunction = metalPS->GetMTLFunction();
                psoDesc.sampleCount = desc.sampleCount;
                psoDesc.colorAttachments[0].pixelFormat = ConvertFormat(desc.renderTargetFormats[0]);
                psoDesc.depthAttachmentPixelFormat = ConvertFormat(desc.depthStencilFormat);

                NSError* error = nil;
                id<MTLRenderPipelineState> renderPipeline = [m_device.Get() newRenderPipelineStateWithDescriptor:psoDesc
                                                                                                           error:&error];
                if (renderPipeline == nil)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Metal pipeline creation failed: %s",
                                    error != nil ? error.localizedDescription.UTF8String : "unknown error");
                    return nullptr;
                }

                MTLDepthStencilDescriptor* depthDesc = [[MTLDepthStencilDescriptor alloc] init];
                depthDesc.depthCompareFunction = ConvertCompareOp(desc.depthStencil.depthFunc);
                depthDesc.depthWriteEnabled = desc.depthStencil.depthWrite ? YES : NO;
                id<MTLDepthStencilState> depthState = [m_device.Get() newDepthStencilStateWithDescriptor:depthDesc];

                return std::make_unique<MetalPipelineState>(desc, renderPipeline, depthState, metalVS, metalPS);
            }

            void* MetalDevice::MapBuffer(IRHIBuffer* buffer)
            {
                auto* metalBuffer = dynamic_cast<MetalBuffer*>(buffer);
                if (metalBuffer == nullptr)
                {
                    return nullptr;
                }
                return [metalBuffer->GetMTLBuffer() contents];
            }

            void MetalDevice::UnmapBuffer(IRHIBuffer* /*buffer*/)
            {
            }

            void MetalDevice::UpdateBuffer(IRHIBuffer* buffer, const void* data, size_t size, size_t offset)
            {
                auto* metalBuffer = dynamic_cast<MetalBuffer*>(buffer);
                if (metalBuffer == nullptr || data == nullptr)
                {
                    return;
                }

                uint8_t* base = reinterpret_cast<uint8_t*>([metalBuffer->GetMTLBuffer() contents]);
                std::memcpy(base + offset, data, size);
            }

            void MetalDevice::UpdateTexture(IRHITexture* texture, const void* data, uint32_t mipLevel,
                                            uint32_t arraySlice)
            {
                auto* metalTexture = dynamic_cast<MetalTexture*>(texture);
                if (metalTexture == nullptr || data == nullptr)
                {
                    return;
                }

                MTLRegion region = MTLRegionMake2D(0, 0, metalTexture->GetWidth(), metalTexture->GetHeight());
                const NSUInteger bytesPerPixel = 4;
                const NSUInteger bytesPerRow = metalTexture->GetWidth() * bytesPerPixel;
                [metalTexture->GetMTLTexture() replaceRegion:region
                                                 mipmapLevel:mipLevel
                                                       slice:arraySlice
                                                   withBytes:data
                                                 bytesPerRow:bytesPerRow
                                               bytesPerImage:bytesPerRow * metalTexture->GetHeight()];
            }

            IRHICommandList* MetalDevice::GetImmediateCommandList()
            {
                return m_immediateCommandList.get();
            }

            std::unique_ptr<IRHICommandList> MetalDevice::CreateDeferredCommandList()
            {
                return std::make_unique<MetalCommandList>(m_commandQueue.Get(), false);
            }

            void MetalDevice::ExecuteCommandList(IRHICommandList* commandList)
            {
                auto* metalCommandList = dynamic_cast<MetalCommandList*>(commandList);
                if (metalCommandList == nullptr || metalCommandList->GetMTLCommandBuffer() == nil)
                {
                    return;
                }

                [metalCommandList->GetMTLCommandBuffer() commit];
                if (metalCommandList->GetMTLCommandBuffer().status == MTLCommandBufferStatusError)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Metal command buffer execution failed");
                }
            }

            void MetalDevice::BeginFrame()
            {
                ++m_statistics.frameCount;
            }

            void MetalDevice::EndFrame()
            {
            }

            void MetalDevice::WaitForIdle()
            {
                if (!m_commandQueue)
                {
                    return;
                }

                id<MTLCommandBuffer> buffer = [m_commandQueue.Get() commandBuffer];
                [buffer commit];
                [buffer waitUntilCompleted];
            }

            void MetalDevice::ResetStatistics()
            {
                m_statistics = {};
            }

            std::string MetalDevice::GetDeviceInfo() const
            {
                if (!m_device)
                {
                    return "Metal (uninitialized)";
                }
                return std::string("Metal: ") + [[m_device.Get() name] UTF8String];
            }

            MetalComputePipelineState* MetalDevice::CreateComputePipeline(MetalShader* computeShader,
                                                                           const std::string& debugName)
            {
                if (computeShader == nullptr)
                {
                    return nullptr;
                }

                NSError* error = nil;
                id<MTLComputePipelineState> state = [m_device.Get()
                    newComputePipelineStateWithFunction:computeShader->GetMTLFunction()
                                                  error:&error];
                if (state == nil)
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Graphics, "Metal compute pipeline failed: %s",
                                    error != nil ? error.localizedDescription.UTF8String : "unknown error");
                    return nullptr;
                }

                return new MetalComputePipelineState(state, debugName);
            }

            MetalArgumentBuffer* MetalDevice::CreateArgumentBuffer(id<MTLFunction> function,
                                                                    uint32_t bufferIndex,
                                                                    uint32_t maxEntries)
            {
                if (function == nil)
                {
                    return nullptr;
                }

                id<MTLArgumentEncoder> encoder = [function newArgumentEncoderWithBufferIndex:bufferIndex];
                if (encoder == nil)
                {
                    return nullptr;
                }

                id<MTLBuffer> argumentBuffer = [m_device.Get() newBufferWithLength:encoder.encodedLength
                                                                            options:MTLResourceStorageModeShared];
                if (argumentBuffer == nil)
                {
                    return nullptr;
                }

                return new MetalArgumentBuffer(argumentBuffer, encoder, maxEntries);
            }

            void MetalDevice::DetectFeatures()
            {
                m_metalFeatures.maxBufferLength = static_cast<uint32_t>(
                    std::min<NSUInteger>(m_device.Get().maxBufferLength, std::numeric_limits<uint32_t>::max()));
                m_metalFeatures.maxThreadgroupMemoryLength = static_cast<uint32_t>(m_device.Get().maxThreadgroupMemoryLength);

#if defined(MTLGPUFamilyApple7)
                m_metalFeatures.appleGPUFamily7 = [m_device.Get() supportsFamily:MTLGPUFamilyApple7];
#endif
#if defined(MTLGPUFamilyApple8)
                m_metalFeatures.appleGPUFamily8 = [m_device.Get() supportsFamily:MTLGPUFamilyApple8];
#endif
#if defined(MTLGPUFamilyApple9)
                m_metalFeatures.appleGPUFamily9 = [m_device.Get() supportsFamily:MTLGPUFamilyApple9];
#endif

                m_metalFeatures.meshShaders = m_metalFeatures.appleGPUFamily7;
                m_metalFeatures.rayTracing = [m_device.Get() supportsRaytracing];
                m_metalFeatures.dynamicLibraries = [m_device.Get() supportsDynamicLibraries];
                m_metalFeatures.functionPointers = [m_device.Get() supportsFunctionPointers];
                m_metalFeatures.maxArgumentBufferEntries = 1024;
            }

            void MetalDevice::PopulateCapabilities()
            {
                m_capabilities.backend = GraphicsBackend::Metal;
                m_capabilities.deviceName = [[m_device.Get() name] UTF8String];
                m_capabilities.vendorName = "Apple";
                m_capabilities.apiVersion = "Metal";
                m_capabilities.maxTextureSize = 16384;
                m_capabilities.maxRenderTargets = 8;
                m_capabilities.maxSamplers = 16;
                m_capabilities.maxConstantBuffers = 31;
                m_capabilities.maxVertexAttributes = 31;
                m_capabilities.computeShaderSupport = true;
                m_capabilities.multiDrawIndirectSupport = true;
                m_capabilities.meshShaderSupport = m_metalFeatures.meshShaders;
                m_capabilities.bindlessResourceSupport = true;
                m_capabilities.rayTracing.bestBackend =
                    m_metalFeatures.rayTracing ? RayTracingBackend::HardwareVKRT : RayTracingBackend::Disabled;
                FinalizeDeviceCapabilities(m_capabilities);
            }

            MTLPixelFormat MetalDevice::ConvertFormat(PixelFormat format) const
            {
                switch (format)
                {
                case PixelFormat::R8_UNORM:
                    return MTLPixelFormatR8Unorm;
                case PixelFormat::R8G8_UNORM:
                    return MTLPixelFormatRG8Unorm;
                case PixelFormat::R16_FLOAT:
                    return MTLPixelFormatR16Float;
                case PixelFormat::R16G16_FLOAT:
                    return MTLPixelFormatRG16Float;
                case PixelFormat::R32_FLOAT:
                    return MTLPixelFormatR32Float;
                case PixelFormat::R32G32_FLOAT:
                    return MTLPixelFormatRG32Float;
                case PixelFormat::R8G8B8A8_UNORM:
                    return MTLPixelFormatRGBA8Unorm;
                case PixelFormat::R8G8B8A8_UNORM_SRGB:
                    return MTLPixelFormatRGBA8Unorm_sRGB;
                case PixelFormat::B8G8R8A8_UNORM:
                    return MTLPixelFormatBGRA8Unorm;
                case PixelFormat::B8G8R8A8_UNORM_SRGB:
                    return MTLPixelFormatBGRA8Unorm_sRGB;
                case PixelFormat::R16G16B16A16_FLOAT:
                    return MTLPixelFormatRGBA16Float;
                case PixelFormat::R32G32B32A32_FLOAT:
                    return MTLPixelFormatRGBA32Float;
                case PixelFormat::D16_UNORM:
                    return MTLPixelFormatDepth16Unorm;
                case PixelFormat::D32_FLOAT:
                    return MTLPixelFormatDepth32Float;
                case PixelFormat::D24_UNORM_S8_UINT:
                    return MTLPixelFormatDepth24Unorm_Stencil8;
                case PixelFormat::D32_FLOAT_S8_UINT:
                    return MTLPixelFormatDepth32Float_Stencil8;
                default:
                    return MTLPixelFormatRGBA8Unorm;
                }
            }

            MTLSamplerMinMagFilter MetalDevice::ConvertMinMagFilter(RHIFilterMode mode) const
            {
                switch (mode)
                {
                case RHIFilterMode::Nearest:
                    return MTLSamplerMinMagFilterNearest;
                case RHIFilterMode::Anisotropic:
                case RHIFilterMode::Linear:
                default:
                    return MTLSamplerMinMagFilterLinear;
                }
            }

            MTLSamplerMipFilter MetalDevice::ConvertMipFilter(RHIFilterMode mode) const
            {
                switch (mode)
                {
                case RHIFilterMode::Nearest:
                    return MTLSamplerMipFilterNearest;
                case RHIFilterMode::Anisotropic:
                case RHIFilterMode::Linear:
                default:
                    return MTLSamplerMipFilterLinear;
                }
            }

            MTLSamplerAddressMode MetalDevice::ConvertAddressMode(RHIAddressMode mode) const
            {
                switch (mode)
                {
                case RHIAddressMode::Clamp:
                    return MTLSamplerAddressModeClampToEdge;
                case RHIAddressMode::Mirror:
                    return MTLSamplerAddressModeMirrorRepeat;
                case RHIAddressMode::Border:
                    return MTLSamplerAddressModeClampToBorderColor;
                case RHIAddressMode::MirrorOnce:
                    return MTLSamplerAddressModeMirrorClampToEdge;
                case RHIAddressMode::Wrap:
                default:
                    return MTLSamplerAddressModeRepeat;
                }
            }

            MTLCompareFunction MetalDevice::ConvertCompareOp(RHICompareOp op) const
            {
                switch (op)
                {
                case RHICompareOp::Never:
                    return MTLCompareFunctionNever;
                case RHICompareOp::Less:
                    return MTLCompareFunctionLess;
                case RHICompareOp::Equal:
                    return MTLCompareFunctionEqual;
                case RHICompareOp::LessEqual:
                    return MTLCompareFunctionLessEqual;
                case RHICompareOp::Greater:
                    return MTLCompareFunctionGreater;
                case RHICompareOp::NotEqual:
                    return MTLCompareFunctionNotEqual;
                case RHICompareOp::GreaterEqual:
                    return MTLCompareFunctionGreaterEqual;
                case RHICompareOp::Always:
                default:
                    return MTLCompareFunctionAlways;
                }
            }

            MTLStencilOperation MetalDevice::ConvertStencilOp(RHIStencilOp op) const
            {
                switch (op)
                {
                case RHIStencilOp::Zero:
                    return MTLStencilOperationZero;
                case RHIStencilOp::Replace:
                    return MTLStencilOperationReplace;
                case RHIStencilOp::IncrSat:
                    return MTLStencilOperationIncrementClamp;
                case RHIStencilOp::DecrSat:
                    return MTLStencilOperationDecrementClamp;
                case RHIStencilOp::Invert:
                    return MTLStencilOperationInvert;
                case RHIStencilOp::IncrWrap:
                    return MTLStencilOperationIncrementWrap;
                case RHIStencilOp::DecrWrap:
                    return MTLStencilOperationDecrementWrap;
                case RHIStencilOp::Keep:
                default:
                    return MTLStencilOperationKeep;
                }
            }

            MTLBlendFactor MetalDevice::ConvertBlendFactor(RHIBlendFactor factor) const
            {
                switch (factor)
                {
                case RHIBlendFactor::Zero:
                    return MTLBlendFactorZero;
                case RHIBlendFactor::One:
                    return MTLBlendFactorOne;
                case RHIBlendFactor::SrcColor:
                    return MTLBlendFactorSourceColor;
                case RHIBlendFactor::InvSrcColor:
                    return MTLBlendFactorOneMinusSourceColor;
                case RHIBlendFactor::SrcAlpha:
                    return MTLBlendFactorSourceAlpha;
                case RHIBlendFactor::InvSrcAlpha:
                    return MTLBlendFactorOneMinusSourceAlpha;
                case RHIBlendFactor::DstAlpha:
                    return MTLBlendFactorDestinationAlpha;
                case RHIBlendFactor::InvDstAlpha:
                    return MTLBlendFactorOneMinusDestinationAlpha;
                case RHIBlendFactor::DstColor:
                    return MTLBlendFactorDestinationColor;
                case RHIBlendFactor::InvDstColor:
                    return MTLBlendFactorOneMinusDestinationColor;
                default:
                    return MTLBlendFactorOne;
                }
            }

            MTLBlendOperation MetalDevice::ConvertBlendOp(RHIBlendOp op) const
            {
                switch (op)
                {
                case RHIBlendOp::Subtract:
                    return MTLBlendOperationSubtract;
                case RHIBlendOp::RevSubtract:
                    return MTLBlendOperationReverseSubtract;
                case RHIBlendOp::Min:
                    return MTLBlendOperationMin;
                case RHIBlendOp::Max:
                    return MTLBlendOperationMax;
                case RHIBlendOp::Add:
                default:
                    return MTLBlendOperationAdd;
                }
            }

            MTLVertexFormat MetalDevice::ConvertVertexFormat(RHIVertexFormat format) const
            {
                switch (format)
                {
                case RHIVertexFormat::Float1:
                    return MTLVertexFormatFloat;
                case RHIVertexFormat::Float2:
                    return MTLVertexFormatFloat2;
                case RHIVertexFormat::Float3:
                    return MTLVertexFormatFloat3;
                case RHIVertexFormat::Float4:
                    return MTLVertexFormatFloat4;
                case RHIVertexFormat::Int1:
                    return MTLVertexFormatInt;
                case RHIVertexFormat::Int2:
                    return MTLVertexFormatInt2;
                case RHIVertexFormat::Int3:
                    return MTLVertexFormatInt3;
                case RHIVertexFormat::Int4:
                    return MTLVertexFormatInt4;
                case RHIVertexFormat::UInt1:
                    return MTLVertexFormatUInt;
                case RHIVertexFormat::UInt2:
                    return MTLVertexFormatUInt2;
                case RHIVertexFormat::UInt3:
                    return MTLVertexFormatUInt3;
                case RHIVertexFormat::UInt4:
                    return MTLVertexFormatUInt4;
                case RHIVertexFormat::UNorm8x4:
                    return MTLVertexFormatUChar4Normalized;
                case RHIVertexFormat::SNorm8x4:
                    return MTLVertexFormatChar4Normalized;
                default:
                    return MTLVertexFormatFloat3;
                }
            }

            MTLResourceOptions MetalDevice::ConvertBufferAccess(RHIBufferAccess access) const
            {
                switch (access)
                {
                case RHIBufferAccess::Static:
                    return MTLResourceStorageModePrivate;
                case RHIBufferAccess::ReadBack:
                    return MTLResourceStorageModeShared | MTLResourceCPUCacheModeDefaultCache;
                case RHIBufferAccess::Staging:
                case RHIBufferAccess::Dynamic:
                default:
                    return MTLResourceStorageModeShared;
                }
            }

            MTLTextureType MetalDevice::ConvertTextureType(RHITextureType type) const
            {
                switch (type)
                {
                case RHITextureType::Texture1D:
                    return MTLTextureType1D;
                case RHITextureType::Texture3D:
                    return MTLTextureType3D;
                case RHITextureType::TextureCube:
                    return MTLTextureTypeCube;
                case RHITextureType::Texture2DArray:
                    return MTLTextureType2DArray;
                case RHITextureType::TextureCubeArray:
                    return MTLTextureTypeCubeArray;
                case RHITextureType::Texture2D:
                default:
                    return MTLTextureType2D;
                }
            }

        } // namespace Metal
    } // namespace RHI
} // namespace Spark

#endif // __APPLE__
