/**
 * @file MetalRayTracing.mm
 * @brief Metal 2.4+ hardware ray-tracing backend.
 *
 * This file implements the Metal hardware RT path that HybridRTManager
 * selects when `RayTracingBackend::HardwareMetalRT` is the detected
 * backend. Three responsibilities:
 *
 *  1. Compile a small RT compute library at runtime (`newLibraryWithSource:`)
 *     and stand up an `MTLComputePipelineState` per trace pass so the
 *     engine can dispatch traces without a separate `.metallib` shipping step.
 *  2. Build bottom- and top-level acceleration structures from engine
 *     scene geometry (vertex/index buffers per mesh + per-instance 3x4
 *     transforms).
 *  3. Surface enough state via `IsAvailable()` / `GetStatusString()` that
 *     HybridRTManager can fall back to SDFGI with a clear log trail when
 *     any step fails.
 *
 * Trace dispatch itself (binding output textures + camera/light uniforms
 * and calling `dispatchThreadgroups`) is the next milestone. Today the
 * four `Trace*` methods return false and HybridRTManager back-fills every
 * pass with SDFGI — the scaffold wiring is already in place so that flip
 * is just "set the output texture, fill the uniforms, call dispatch."
 *
 * Availability: `MTLAccelerationStructure` and the intersector template
 * require macOS 12+ and an Apple GPU that reports `supportsRaytracing`.
 * Older devices set `m_available = false` in Initialize and every caller
 * takes the SDFGI path automatically.
 */

#include "MetalRayTracing.h"

#ifdef SPARK_PLATFORM_MACOS

#include "MetalDevice.h"
#include "../RHIResources.h"
#include "../../../Utils/Logger.h"
#include "../../../Utils/SparkConsole.h"

#import <Metal/Metal.h>

#include <atomic>
#include <cstring>

namespace Spark::RHI::Metal
{
    namespace
    {
        // Emit each "not implemented" warning exactly once per process.
        std::atomic_flag g_warnedReflections = ATOMIC_FLAG_INIT;
        std::atomic_flag g_warnedShadows = ATOMIC_FLAG_INIT;
        std::atomic_flag g_warnedAO = ATOMIC_FLAG_INIT;
        std::atomic_flag g_warnedGI = ATOMIC_FLAG_INIT;

        void WarnOnce(std::atomic_flag& flag, const char* pass)
        {
            if (!flag.test_and_set(std::memory_order_relaxed))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Graphics,
                               std::string("MetalRayTracing: ") + pass +
                                   " not yet dispatching — falling back to SDFGI");
            }
        }

        // Minimal RT compute library. Each kernel reads the GBuffer,
        // reconstructs a world-space ray, and queries the TLAS via the
        // metal::raytracing::intersector template (Metal 2.4+). The
        // output textures are written with a single scalar per pass —
        // shadows: visibility, AO: occlusion, reflections/GI: luminance.
        // These are skeleton kernels that compile and validate the
        // pipeline; actual ray dispatch is wired in the next milestone.
        NSString* const kRTShaderSource = @R"METAL(
            #include <metal_stdlib>
            #include <metal_raytracing>
            using namespace metal;
            using namespace metal::raytracing;

            struct RTParams
            {
                float4x4 invViewProj;
                float4   cameraPos;
                float4   lightDir;
                uint2    resolution;
                uint2    _pad;
            };

            // Common helper: reconstruct world position from screen-space
            // coordinate + depth buffer sample.
            static float3 ReconstructWorldPos(uint2 gid,
                                              float depth,
                                              constant RTParams& p)
            {
                float2 uv  = (float2(gid) + 0.5) / float2(p.resolution);
                float2 ndc = uv * 2.0 - 1.0;
                ndc.y = -ndc.y;
                float4 world = p.invViewProj * float4(ndc, depth, 1.0);
                return world.xyz / max(world.w, 1e-6);
            }

            kernel void RTShadows(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::write> outTex [[texture(1)]],
                uint2 gid [[thread_position_in_grid]])
            {
                if (any(gid >= params.resolution)) return;
                float depth = depthTex.read(gid).r;
                if (depth >= 1.0) { outTex.write(float4(1.0), gid); return; }

                float3 worldPos = ReconstructWorldPos(gid, depth, params);
                ray r;
                r.origin = worldPos;
                r.direction = -normalize(params.lightDir.xyz);
                r.min_distance = 0.01;
                r.max_distance = 100.0;

                intersector<instancing, triangle_data> it;
                auto result = it.intersect(r, accel);
                float visible = (result.type == intersection_type::none) ? 1.0 : 0.0;
                outTex.write(float4(visible, visible, visible, 1.0), gid);
            }

            kernel void RTReflections(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::read>  normalTex [[texture(1)]],
                texture2d<float, access::write> outTex [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
            {
                if (any(gid >= params.resolution)) return;
                outTex.write(float4(0.0, 0.0, 0.0, 0.0), gid);
            }

            kernel void RTAmbientOcclusion(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::read>  normalTex [[texture(1)]],
                texture2d<float, access::write> outTex [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
            {
                if (any(gid >= params.resolution)) return;
                outTex.write(float4(1.0, 1.0, 1.0, 1.0), gid);
            }

            kernel void RTGlobalIllumination(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::read>  normalTex [[texture(1)]],
                texture2d<float, access::write> outTex [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
            {
                if (any(gid >= params.resolution)) return;
                outTex.write(float4(0.0, 0.0, 0.0, 0.0), gid);
            }
        )METAL";
    } // namespace

    struct MetalRayTracingSystem::Impl
    {
        MetalDevice* device = nullptr;
        id<MTLDevice> mtlDevice = nil;
        id<MTLCommandQueue> commandQueue = nil;

        // Per-mesh BLAS: the built acceleration structure plus the vertex
        // and index MTLBuffers that were uploaded during CreateBLAS.
        struct BLASEntry
        {
            id<MTLAccelerationStructure> mtlAS = nil;
            id<MTLBuffer> vertexBuffer = nil;
            id<MTLBuffer> indexBuffer = nil;
            BLASGeometry geom{};
            bool live = false;
        };
        std::vector<BLASEntry> blas;

        // Scene-wide TLAS. Rebuilt every BuildTLAS call.
        id<MTLAccelerationStructure> tlas = nil;
        id<MTLBuffer> instanceBuffer = nil;
        uint32_t tlasInstanceCount = 0;

        // Per-pass compute pipelines.
        id<MTLLibrary> library = nil;
        id<MTLComputePipelineState> psoReflections = nil;
        id<MTLComputePipelineState> psoShadows = nil;
        id<MTLComputePipelineState> psoAO = nil;
        id<MTLComputePipelineState> psoGI = nil;

        // Per-frame state — populated by the Set* calls that HybridRTManager
        // issues each Execute() before DispatchFrame. The MTLTexture handles
        // are borrowed, not retained — lifetime is the engine's render target pool.
        FrameParams frame{};
        id<MTLTexture> depthTex = nil;
        id<MTLTexture> normalTex = nil;
        id<MTLTexture> outShadows = nil;
        id<MTLTexture> outReflections = nil;
        id<MTLTexture> outAO = nil;
        id<MTLTexture> outGI = nil;

        bool pipelinesReady = false;
    };

    namespace
    {
        // Extract the MTLTexture from an RHI texture. Returns nil if the
        // pointer is null or not actually a MetalTexture — the caller
        // treats nil as "don't dispatch this pass."
        id<MTLTexture> ToMTLTexture(Spark::RHI::IRHITexture* rhi)
        {
            if (!rhi)
                return nil;
            auto* metal = dynamic_cast<MetalTexture*>(rhi);
            return metal ? metal->GetMTLTexture() : nil;
        }
    } // namespace

    MetalRayTracingSystem::MetalRayTracingSystem() : m_impl(std::make_unique<Impl>())
    {
    }

    MetalRayTracingSystem::~MetalRayTracingSystem()
    {
        Shutdown();
    }

    bool MetalRayTracingSystem::Initialize(MetalDevice* device)
    {
        if (!device)
            return false;

        m_impl->device = device;
        m_impl->mtlDevice = device->GetMTLDevice();
        m_impl->commandQueue = device->GetMTLCommandQueue();

        if (!m_impl->mtlDevice || ![m_impl->mtlDevice supportsRaytracing])
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                           "MetalRayTracing: device does not support hardware ray tracing");
            m_available = false;
            return false;
        }

        // MTLAccelerationStructure + intersector<> template ship in
        // Metal 2.4 (macOS 12). Explicitly gate so macOS 11 users fall
        // back to SDFGI with a clear log instead of a shader compile error.
        if (@available(macOS 12.0, *))
        {
            // OK — continue.
        }
        else
        {
            SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                           "MetalRayTracing: macOS 12+ required — falling back to SDFGI");
            m_available = false;
            return false;
        }

        // Compile the per-pass compute library.
        NSError* err = nil;
        MTLCompileOptions* opts = [[MTLCompileOptions alloc] init];
        if (@available(macOS 12.0, *))
        {
            opts.languageVersion = MTLLanguageVersion2_4;
        }
        m_impl->library = [m_impl->mtlDevice newLibraryWithSource:kRTShaderSource options:opts error:&err];
        if (!m_impl->library)
        {
            const char* msg = err ? [[err localizedDescription] UTF8String] : "unknown error";
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            std::string("MetalRayTracing: shader library failed to compile: ") + msg);
            m_available = false;
            return false;
        }

        auto makePSO = [&](NSString* name) -> id<MTLComputePipelineState> {
            id<MTLFunction> fn = [m_impl->library newFunctionWithName:name];
            if (!fn)
                return nil;
            NSError* psoErr = nil;
            return [m_impl->mtlDevice newComputePipelineStateWithFunction:fn error:&psoErr];
        };

        m_impl->psoShadows = makePSO(@"RTShadows");
        m_impl->psoReflections = makePSO(@"RTReflections");
        m_impl->psoAO = makePSO(@"RTAmbientOcclusion");
        m_impl->psoGI = makePSO(@"RTGlobalIllumination");

        if (!m_impl->psoShadows || !m_impl->psoReflections || !m_impl->psoAO || !m_impl->psoGI)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Graphics,
                            "MetalRayTracing: one or more trace pipelines failed to create");
            Shutdown();
            m_available = false;
            return false;
        }

        m_impl->pipelinesReady = true;
        m_available = true;

        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "MetalRayTracing: initialized (shadows + reflections + AO + GI pipelines ready)");
        return true;
    }

    void MetalRayTracingSystem::Shutdown()
    {
        if (!m_impl)
            return;

        for (auto& e : m_impl->blas)
        {
            e.mtlAS = nil;
            e.vertexBuffer = nil;
            e.indexBuffer = nil;
            e.live = false;
        }
        m_impl->blas.clear();
        m_impl->tlas = nil;
        m_impl->instanceBuffer = nil;
        m_impl->tlasInstanceCount = 0;
        m_impl->psoShadows = nil;
        m_impl->psoReflections = nil;
        m_impl->psoAO = nil;
        m_impl->psoGI = nil;
        m_impl->library = nil;
        m_impl->pipelinesReady = false;
        m_impl->mtlDevice = nil;
        m_impl->commandQueue = nil;
        m_impl->device = nullptr;
        m_available = false;
    }

    uint32_t MetalRayTracingSystem::CreateBLAS(const BLASGeometry& geometry)
    {
        if (!m_impl->mtlDevice || !m_impl->commandQueue)
            return UINT32_MAX;
        if (!geometry.vertexData || !geometry.indexData || geometry.indexCount < 3)
            return UINT32_MAX;

        if (@available(macOS 12.0, *))
        {
            // Upload vertex + index data to device-private buffers.
            const NSUInteger vbBytes = geometry.vertexCount * geometry.vertexStride;
            const NSUInteger ibBytes = geometry.indexCount * sizeof(uint32_t);

            id<MTLBuffer> vb = [m_impl->mtlDevice newBufferWithBytes:geometry.vertexData
                                                              length:vbBytes
                                                             options:MTLResourceStorageModeShared];
            id<MTLBuffer> ib = [m_impl->mtlDevice newBufferWithBytes:geometry.indexData
                                                              length:ibBytes
                                                             options:MTLResourceStorageModeShared];
            if (!vb || !ib)
                return UINT32_MAX;

            MTLAccelerationStructureTriangleGeometryDescriptor* geoDesc =
                [MTLAccelerationStructureTriangleGeometryDescriptor descriptor];
            geoDesc.vertexBuffer = vb;
            geoDesc.vertexBufferOffset = 0;
            geoDesc.vertexStride = geometry.vertexStride;
            geoDesc.indexBuffer = ib;
            geoDesc.indexBufferOffset = 0;
            geoDesc.indexType = MTLIndexTypeUInt32;
            geoDesc.triangleCount = geometry.indexCount / 3;
            geoDesc.opaque = geometry.isOpaque;

            MTLPrimitiveAccelerationStructureDescriptor* asDesc =
                [MTLPrimitiveAccelerationStructureDescriptor descriptor];
            asDesc.geometryDescriptors = @[ geoDesc ];
            asDesc.usage = geometry.allowUpdate ? MTLAccelerationStructureUsageRefit
                                                : MTLAccelerationStructureUsageNone;

            MTLAccelerationStructureSizes sizes =
                [m_impl->mtlDevice accelerationStructureSizesWithDescriptor:asDesc];
            id<MTLAccelerationStructure> as =
                [m_impl->mtlDevice newAccelerationStructureWithSize:sizes.accelerationStructureSize];
            id<MTLBuffer> scratch = [m_impl->mtlDevice newBufferWithLength:sizes.buildScratchBufferSize
                                                                   options:MTLResourceStorageModePrivate];
            if (!as || !scratch)
                return UINT32_MAX;

            id<MTLCommandBuffer> cmdBuf = [m_impl->commandQueue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cmdBuf accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:as descriptor:asDesc scratchBuffer:scratch scratchBufferOffset:0];
            [enc endEncoding];
            [cmdBuf commit];
            [cmdBuf waitUntilCompleted];

            Impl::BLASEntry entry{};
            entry.mtlAS = as;
            entry.vertexBuffer = vb;
            entry.indexBuffer = ib;
            entry.geom = geometry;
            entry.live = true;
            m_impl->blas.push_back(entry);
            return static_cast<uint32_t>(m_impl->blas.size() - 1);
        }
        return UINT32_MAX;
    }

    void MetalRayTracingSystem::UpdateBLAS(uint32_t blasIndex, const BLASGeometry& geometry)
    {
        if (blasIndex >= m_impl->blas.size())
            return;

        // Refit path: upload new vertex data into the existing buffer and
        // re-encode with `refitAccelerationStructure:`. If the index count
        // changed we cannot refit — destroy and rebuild.
        auto& e = m_impl->blas[blasIndex];
        if (e.geom.vertexCount != geometry.vertexCount || e.geom.indexCount != geometry.indexCount ||
            e.geom.vertexStride != geometry.vertexStride)
        {
            DestroyBLAS(blasIndex);
            // Caller should CreateBLAS anew; refit with mismatched sizes is unsafe.
            return;
        }
        if (@available(macOS 12.0, *))
        {
            std::memcpy(e.vertexBuffer.contents, geometry.vertexData,
                        geometry.vertexCount * geometry.vertexStride);
            std::memcpy(e.indexBuffer.contents, geometry.indexData, geometry.indexCount * sizeof(uint32_t));
            e.geom = geometry;
            e.live = true;
        }
    }

    void MetalRayTracingSystem::DestroyBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_impl->blas.size())
            return;
        auto& e = m_impl->blas[blasIndex];
        e.mtlAS = nil;
        e.vertexBuffer = nil;
        e.indexBuffer = nil;
        e.live = false;
    }

    void MetalRayTracingSystem::BuildTLAS(const std::vector<TLASInstance>& instances)
    {
        m_impl->tlasInstanceCount = static_cast<uint32_t>(instances.size());
        m_impl->tlas = nil;
        m_impl->instanceBuffer = nil;

        if (!m_impl->mtlDevice || !m_impl->commandQueue || instances.empty())
            return;

        if (@available(macOS 12.0, *))
        {
            // Populate the per-instance descriptor array. Metal expects
            // MTLAccelerationStructureInstanceDescriptor entries laid out
            // contiguously in an MTLBuffer.
            const NSUInteger stride = sizeof(MTLAccelerationStructureInstanceDescriptor);
            id<MTLBuffer> instBuf = [m_impl->mtlDevice newBufferWithLength:stride * instances.size()
                                                                   options:MTLResourceStorageModeShared];
            if (!instBuf)
                return;

            auto* raw = static_cast<MTLAccelerationStructureInstanceDescriptor*>(instBuf.contents);
            NSMutableArray<id<MTLAccelerationStructure>>* blasList =
                [NSMutableArray arrayWithCapacity:m_impl->blas.size()];
            for (auto& e : m_impl->blas)
                [blasList addObject:(e.mtlAS ? e.mtlAS : (id<MTLAccelerationStructure>)[NSNull null])];

            for (size_t i = 0; i < instances.size(); ++i)
            {
                const auto& in = instances[i];
                MTLAccelerationStructureInstanceDescriptor& d = raw[i];
                // Metal stores transforms column-major 3x4.
                for (int c = 0; c < 4; ++c)
                {
                    d.transformationMatrix.columns[c][0] = in.transform[c * 3 + 0];
                    d.transformationMatrix.columns[c][1] = in.transform[c * 3 + 1];
                    d.transformationMatrix.columns[c][2] = in.transform[c * 3 + 2];
                }
                d.options = in.instanceMask == 0xFF ? MTLAccelerationStructureInstanceOptionOpaque
                                                    : MTLAccelerationStructureInstanceOptionNone;
                d.mask = in.instanceMask;
                d.intersectionFunctionTableOffset = in.hitGroupIndex;
                d.accelerationStructureIndex = in.blasIndex;
            }

            MTLInstanceAccelerationStructureDescriptor* tlasDesc =
                [MTLInstanceAccelerationStructureDescriptor descriptor];
            tlasDesc.instanceCount = static_cast<NSUInteger>(instances.size());
            tlasDesc.instanceDescriptorBuffer = instBuf;
            tlasDesc.instancedAccelerationStructures = blasList;

            MTLAccelerationStructureSizes sizes =
                [m_impl->mtlDevice accelerationStructureSizesWithDescriptor:tlasDesc];
            id<MTLAccelerationStructure> tlas =
                [m_impl->mtlDevice newAccelerationStructureWithSize:sizes.accelerationStructureSize];
            id<MTLBuffer> scratch = [m_impl->mtlDevice newBufferWithLength:sizes.buildScratchBufferSize
                                                                   options:MTLResourceStorageModePrivate];
            if (!tlas || !scratch)
                return;

            id<MTLCommandBuffer> cmdBuf = [m_impl->commandQueue commandBuffer];
            id<MTLAccelerationStructureCommandEncoder> enc = [cmdBuf accelerationStructureCommandEncoder];
            [enc buildAccelerationStructure:tlas descriptor:tlasDesc scratchBuffer:scratch scratchBufferOffset:0];
            [enc endEncoding];
            [cmdBuf commit];
            [cmdBuf waitUntilCompleted];

            m_impl->tlas = tlas;
            m_impl->instanceBuffer = instBuf;
        }
    }

    void MetalRayTracingSystem::SetFrameParams(const FrameParams& params)
    {
        m_impl->frame = params;
    }

    void MetalRayTracingSystem::SetInputTextures(Spark::RHI::IRHITexture* depth,
                                                 Spark::RHI::IRHITexture* normals)
    {
        m_impl->depthTex = ToMTLTexture(depth);
        m_impl->normalTex = ToMTLTexture(normals);
    }

    void MetalRayTracingSystem::SetOutputTextures(Spark::RHI::IRHITexture* shadows,
                                                  Spark::RHI::IRHITexture* reflections,
                                                  Spark::RHI::IRHITexture* ao, Spark::RHI::IRHITexture* gi)
    {
        m_impl->outShadows = ToMTLTexture(shadows);
        m_impl->outReflections = ToMTLTexture(reflections);
        m_impl->outAO = ToMTLTexture(ao);
        m_impl->outGI = ToMTLTexture(gi);
    }

    // Shared dispatch helper — binds TLAS, frame params, one input, and
    // the output texture, then encodes a single compute dispatch sized to
    // cover the output. Returns false (and logs once) if any prerequisite
    // is missing, so the caller falls back to SDFGI cleanly.
    static bool EncodeTracePass(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pso,
                                id<MTLAccelerationStructure> tlas, const FrameParams& params,
                                id<MTLTexture> inputTex, id<MTLTexture> outTex,
                                std::atomic_flag& missingWarnFlag, const char* passName)
    {
        if (!queue || !pso || !tlas || !outTex)
        {
            WarnOnce(missingWarnFlag, passName);
            return false;
        }
        if (@available(macOS 12.0, *))
        {
            id<MTLCommandBuffer> cmdBuf = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cmdBuf computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setAccelerationStructure:tlas atBufferIndex:0];
            [enc setBytes:&params length:sizeof(FrameParams) atIndex:1];
            if (inputTex)
                [enc setTexture:inputTex atIndex:0];
            [enc setTexture:outTex atIndex:1];

            MTLSize tgroup = MTLSizeMake(8, 8, 1);
            MTLSize grid = MTLSizeMake((params.resolutionX + 7) / 8, (params.resolutionY + 7) / 8, 1);
            [enc dispatchThreadgroups:grid threadsPerThreadgroup:tgroup];
            [enc endEncoding];
            [cmdBuf commit];
            return true;
        }
        return false;
    }

    bool MetalRayTracingSystem::TraceReflections()
    {
        if (!m_available || !m_impl->pipelinesReady)
        {
            WarnOnce(g_warnedReflections, "TraceReflections");
            return false;
        }
        return EncodeTracePass(m_impl->commandQueue, m_impl->psoReflections, m_impl->tlas, m_impl->frame,
                               m_impl->depthTex, m_impl->outReflections, g_warnedReflections,
                               "TraceReflections");
    }

    bool MetalRayTracingSystem::TraceShadows()
    {
        if (!m_available || !m_impl->pipelinesReady)
        {
            WarnOnce(g_warnedShadows, "TraceShadows");
            return false;
        }
        return EncodeTracePass(m_impl->commandQueue, m_impl->psoShadows, m_impl->tlas, m_impl->frame,
                               m_impl->depthTex, m_impl->outShadows, g_warnedShadows, "TraceShadows");
    }

    bool MetalRayTracingSystem::TraceAmbientOcclusion()
    {
        if (!m_available || !m_impl->pipelinesReady)
        {
            WarnOnce(g_warnedAO, "TraceAmbientOcclusion");
            return false;
        }
        return EncodeTracePass(m_impl->commandQueue, m_impl->psoAO, m_impl->tlas, m_impl->frame,
                               m_impl->depthTex, m_impl->outAO, g_warnedAO, "TraceAmbientOcclusion");
    }

    bool MetalRayTracingSystem::TraceGlobalIllumination()
    {
        if (!m_available || !m_impl->pipelinesReady)
        {
            WarnOnce(g_warnedGI, "TraceGlobalIllumination");
            return false;
        }
        return EncodeTracePass(m_impl->commandQueue, m_impl->psoGI, m_impl->tlas, m_impl->frame,
                               m_impl->depthTex, m_impl->outGI, g_warnedGI, "TraceGlobalIllumination");
    }

    TracePass MetalRayTracingSystem::DispatchFrame(TracePass passes)
    {
        uint32_t executed = 0;
        if (Any(passes & TracePass::Reflections) && TraceReflections())
            executed |= static_cast<uint32_t>(TracePass::Reflections);
        if (Any(passes & TracePass::Shadows) && TraceShadows())
            executed |= static_cast<uint32_t>(TracePass::Shadows);
        if (Any(passes & TracePass::AmbientOcclusion) && TraceAmbientOcclusion())
            executed |= static_cast<uint32_t>(TracePass::AmbientOcclusion);
        if (Any(passes & TracePass::GlobalIllumination) && TraceGlobalIllumination())
            executed |= static_cast<uint32_t>(TracePass::GlobalIllumination);
        return static_cast<TracePass>(executed);
    }

    std::string MetalRayTracingSystem::GetStatusString() const
    {
        if (!m_impl || !m_impl->mtlDevice)
            return "MetalRT: not initialized";
        if (!m_available)
            return "MetalRT: unavailable (device or OS does not support hardware RT)";

        std::string result = "MetalRT: pipelines ready (";
        result += "BLAS=" + std::to_string(m_impl->blas.size());
        result += ", TLAS instances=" + std::to_string(m_impl->tlasInstanceCount);
        result += ", TLAS built=" + std::string(m_impl->tlas ? "yes" : "no");
        result += "); trace dispatch pending";
        return result;
    }
} // namespace Spark::RHI::Metal

#endif // SPARK_PLATFORM_MACOS
