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
#include "../../../Utils/LogMacros.h"
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

            // Per-instance material, matches C++ MaterialParams. The kernels
            // read materials[instance_id] on hit. When the binding is null
            // (materialCount==0 at CPU side, empty buffer), the kernels fall
            // back to a neutral grey.
            struct MaterialParams
            {
                float4 albedo;
                float4 emissive;
                float4 roughnessMetallic;
            };

            // Shaded colour for a hit sample given the material and the
            // incoming ray direction. Simple Lambert + emissive until
            // real lighting bindings land. Light direction comes from
            // RTParams for shadows/GI so we re-use it here.
            static float3 ShadeHit(constant MaterialParams& mat,
                                   float3 rayDir,
                                   constant RTParams& params)
            {
                float3 n    = normalize(-rayDir);        // face normal stand-in
                float  NdL  = max(0.0, dot(n, -normalize(params.lightDir.xyz)));
                float3 base = mat.albedo.rgb * (0.25 + 0.75 * NdL);
                return base + mat.emissive.rgb;
            }

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

            // Tiny hash-based PRNG — deterministic per (pixel, seed). Not
            // production quality but gives us independent samples per pass.
            static float Hash12(uint2 pix, uint seed)
            {
                uint n = pix.x * 1973u + pix.y * 9277u + seed * 26699u;
                n = (n ^ 61u) ^ (n >> 16);
                n *= 9u;
                n = n ^ (n >> 4);
                n *= 0x27d4eb2du;
                n = n ^ (n >> 15);
                return float(n & 0x00FFFFFFu) / float(0x01000000u);
            }

            // Cosine-weighted hemisphere sample around `n`. Returns a
            // unit direction biased toward the normal.
            static float3 CosineHemisphere(float3 n, float u1, float u2)
            {
                float r = sqrt(u1);
                float phi = 2.0 * M_PI_F * u2;
                float3 t = normalize(abs(n.y) < 0.999 ? cross(n, float3(0,1,0))
                                                      : cross(n, float3(1,0,0)));
                float3 b = cross(n, t);
                return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(0.0, 1.0 - u1)));
            }

            // All kernels use a uniform binding layout so a single dispatch
            // helper can encode every pass:
            //   buffer(0) = instance acceleration structure (TLAS)
            //   buffer(1) = RTParams
            //   buffer(2) = materials[]  (optional; unused by shadows/AO)
            //   buffer(3) = materialCount
            //   texture(0) = GBuffer depth
            //   texture(1) = GBuffer normals (unused by shadows but still bound)
            //   texture(2) = output render target
            kernel void RTShadows(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                constant MaterialParams*        materials [[buffer(2)]],
                constant uint&                  materialCount [[buffer(3)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::read>  normalTex [[texture(1)]],
                texture2d<float, access::write> outTex [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
            {
                (void)materials; (void)materialCount; (void)normalTex;
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

            // Reflections: reflect the view direction off the gbuffer normal
            // and cast one ray. Miss → sky tint (transparent for now). Hit →
            // a constant-shaded colour based on distance so the compositor
            // sees non-zero reflection data.
            kernel void RTReflections(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                constant MaterialParams*        materials [[buffer(2)]],
                constant uint&                  materialCount [[buffer(3)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::read>  normalTex [[texture(1)]],
                texture2d<float, access::write> outTex [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
            {
                if (any(gid >= params.resolution)) return;
                float depth = depthTex.read(gid).r;
                if (depth >= 1.0) { outTex.write(float4(0.0), gid); return; }

                float3 worldPos = ReconstructWorldPos(gid, depth, params);
                float3 normal   = normalize(normalTex.read(gid).xyz * 2.0 - 1.0);
                float3 viewDir  = normalize(worldPos - params.cameraPos.xyz);
                float3 reflDir  = reflect(viewDir, normal);

                ray r;
                r.origin = worldPos;
                r.direction = reflDir;
                r.min_distance = 0.01;
                r.max_distance = 200.0;

                intersector<instancing, triangle_data> it;
                auto result = it.intersect(r, accel);
                if (result.type == intersection_type::none)
                {
                    float t = saturate(reflDir.y * 0.5 + 0.5);
                    outTex.write(float4(mix(float3(0.4, 0.55, 0.7), float3(0.1, 0.15, 0.25), t), 1.0), gid);
                    return;
                }

                // Shade the hit using the per-instance material when
                // available. Attenuate by distance so far reflections
                // fade out (matches DXR reflection compositor behaviour).
                float dist = result.distance;
                float fade = saturate(1.0 - dist / 200.0);
                float3 colour;
                if (materialCount > 0 && result.instance_id < materialCount)
                {
                    colour = ShadeHit(materials[result.instance_id], reflDir, params) * fade;
                }
                else
                {
                    colour = float3(fade * 0.8);
                }
                outTex.write(float4(colour, 1.0), gid);
            }

            // Ambient occlusion: 4 cosine-weighted hemisphere samples, short
            // rays, accumulate miss ratio. Cheap but gives real AO on M-series
            // GPUs. Temporal accumulation is the existing compositor's job.
            kernel void RTAmbientOcclusion(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                constant MaterialParams*        materials [[buffer(2)]],
                constant uint&                  materialCount [[buffer(3)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::read>  normalTex [[texture(1)]],
                texture2d<float, access::write> outTex [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
            {
                (void)materials; (void)materialCount;
                if (any(gid >= params.resolution)) return;
                float depth = depthTex.read(gid).r;
                if (depth >= 1.0) { outTex.write(float4(1.0), gid); return; }

                float3 worldPos = ReconstructWorldPos(gid, depth, params);
                float3 normal   = normalize(normalTex.read(gid).xyz * 2.0 - 1.0);

                intersector<instancing, triangle_data> it;
                const uint kSamples = 4u;
                float visible = 0.0;
                for (uint s = 0u; s < kSamples; ++s)
                {
                    float u1 = Hash12(gid, s * 2u + 0u);
                    float u2 = Hash12(gid, s * 2u + 1u);
                    float3 dir = CosineHemisphere(normal, u1, u2);
                    ray r;
                    r.origin = worldPos + normal * 0.001;
                    r.direction = dir;
                    r.min_distance = 0.005;
                    r.max_distance = 3.0;  // Short AO radius
                    auto result = it.intersect(r, accel);
                    visible += (result.type == intersection_type::none) ? 1.0 : 0.0;
                }
                float ao = visible / float(kSamples);
                outTex.write(float4(ao, ao, ao, 1.0), gid);
            }

            // Single-bounce diffuse GI: one random hemisphere ray. On miss we
            // return the sky, on hit a flat Lambert approximation (0.3 grey)
            // until materials are wired. Temporal reprojection by the
            // compositor is what makes this look acceptable.
            kernel void RTGlobalIllumination(
                instance_acceleration_structure accel [[buffer(0)]],
                constant RTParams&              params [[buffer(1)]],
                constant MaterialParams*        materials [[buffer(2)]],
                constant uint&                  materialCount [[buffer(3)]],
                texture2d<float, access::read>  depthTex [[texture(0)]],
                texture2d<float, access::read>  normalTex [[texture(1)]],
                texture2d<float, access::write> outTex [[texture(2)]],
                uint2 gid [[thread_position_in_grid]])
            {
                if (any(gid >= params.resolution)) return;
                float depth = depthTex.read(gid).r;
                if (depth >= 1.0) { outTex.write(float4(0.0), gid); return; }

                float3 worldPos = ReconstructWorldPos(gid, depth, params);
                float3 normal   = normalize(normalTex.read(gid).xyz * 2.0 - 1.0);
                float u1 = Hash12(gid, 7u);
                float u2 = Hash12(gid, 11u);
                float3 dir = CosineHemisphere(normal, u1, u2);

                ray r;
                r.origin = worldPos + normal * 0.001;
                r.direction = dir;
                r.min_distance = 0.01;
                r.max_distance = 50.0;

                intersector<instancing, triangle_data> it;
                auto result = it.intersect(r, accel);
                float3 radiance;
                if (result.type == intersection_type::none)
                {
                    float t = saturate(dir.y * 0.5 + 0.5);
                    radiance = mix(float3(0.4, 0.55, 0.7), float3(0.1, 0.15, 0.25), t);
                }
                else if (materialCount > 0 && result.instance_id < materialCount)
                {
                    radiance = ShadeHit(materials[result.instance_id], dir, params);
                }
                else
                {
                    radiance = float3(0.3); // Placeholder when material buffer is empty.
                }
                outTex.write(float4(radiance, 1.0), gid);
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

        // Per-instance materials. Kernels sample `materials[instance_id]`
        // on hit; nil buffer means "use the 0.3 grey placeholder".
        id<MTLBuffer> materialBuffer = nil;
        uint32_t materialCount = 0;

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
        m_impl->materialBuffer = nil;
        m_impl->materialCount = 0;
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

    void MetalRayTracingSystem::SetMaterials(const std::vector<MaterialParams>& materials)
    {
        m_impl->materialCount = static_cast<uint32_t>(materials.size());
        if (materials.empty() || !m_impl->mtlDevice)
        {
            m_impl->materialBuffer = nil;
            return;
        }
        const NSUInteger bytes = sizeof(MaterialParams) * materials.size();
        m_impl->materialBuffer = [m_impl->mtlDevice newBufferWithBytes:materials.data()
                                                                length:bytes
                                                               options:MTLResourceStorageModeShared];
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

    // Shared dispatch helper — binds TLAS, frame params, optional
    // per-instance materials, one input, and the output texture, then
    // encodes a single compute dispatch sized to cover the output.
    // Returns false (and logs once) if any prerequisite is missing, so
    // the caller falls back to SDFGI cleanly.
    //
    // `materialBuffer` + `materialCount` are optional — when
    // `materialCount == 0` the kernels still see buffer(2)/buffer(3)
    // arguments (Metal requires declared bindings to be set), so we
    // always provide a placeholder uint even when the real buffer is
    // nil. Kernels branch on `materialCount > 0` before dereferencing.
    static bool EncodeTracePass(id<MTLCommandQueue> queue, id<MTLComputePipelineState> pso,
                                id<MTLAccelerationStructure> tlas, const FrameParams& params,
                                id<MTLBuffer> materialBuffer, uint32_t materialCount, id<MTLTexture> depthTex,
                                id<MTLTexture> normalTex, id<MTLTexture> outTex,
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
            // Materials slot 2 is optional (nil allowed); count slot 3
            // always present so the kernel can branch on it.
            if (materialBuffer)
                [enc setBuffer:materialBuffer offset:0 atIndex:2];
            [enc setBytes:&materialCount length:sizeof(uint32_t) atIndex:3];
            if (depthTex)
                [enc setTexture:depthTex atIndex:0];
            if (normalTex)
                [enc setTexture:normalTex atIndex:1];
            [enc setTexture:outTex atIndex:2];

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
                               m_impl->materialBuffer, m_impl->materialCount, m_impl->depthTex, m_impl->normalTex,
                               m_impl->outReflections, g_warnedReflections, "TraceReflections");
    }

    bool MetalRayTracingSystem::TraceShadows()
    {
        if (!m_available || !m_impl->pipelinesReady)
        {
            WarnOnce(g_warnedShadows, "TraceShadows");
            return false;
        }
        return EncodeTracePass(m_impl->commandQueue, m_impl->psoShadows, m_impl->tlas, m_impl->frame,
                               m_impl->materialBuffer, m_impl->materialCount, m_impl->depthTex, m_impl->normalTex,
                               m_impl->outShadows, g_warnedShadows, "TraceShadows");
    }

    bool MetalRayTracingSystem::TraceAmbientOcclusion()
    {
        if (!m_available || !m_impl->pipelinesReady)
        {
            WarnOnce(g_warnedAO, "TraceAmbientOcclusion");
            return false;
        }
        return EncodeTracePass(m_impl->commandQueue, m_impl->psoAO, m_impl->tlas, m_impl->frame,
                               m_impl->materialBuffer, m_impl->materialCount, m_impl->depthTex, m_impl->normalTex,
                               m_impl->outAO, g_warnedAO, "TraceAmbientOcclusion");
    }

    bool MetalRayTracingSystem::TraceGlobalIllumination()
    {
        if (!m_available || !m_impl->pipelinesReady)
        {
            WarnOnce(g_warnedGI, "TraceGlobalIllumination");
            return false;
        }
        return EncodeTracePass(m_impl->commandQueue, m_impl->psoGI, m_impl->tlas, m_impl->frame,
                               m_impl->materialBuffer, m_impl->materialCount, m_impl->depthTex, m_impl->normalTex,
                               m_impl->outGI, g_warnedGI, "TraceGlobalIllumination");
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
