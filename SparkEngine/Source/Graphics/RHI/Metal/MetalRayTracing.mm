/**
 * @file MetalRayTracing.mm
 * @brief Metal 2.4+ hardware ray-tracing backend — scaffolding.
 *
 * Current status: this is the scaffold-and-wire step for the Metal RT path.
 * The PIMPL holds the MTLDevice / command queue, a flat BLAS table, and a
 * slot for the TLAS and per-effect compute pipelines. No real acceleration-
 * structure build or trace dispatch has been implemented yet; every call
 * returns false / UINT32_MAX / empty so the HybridRTManager falls back to
 * SDFGI for the actual frame rendering.
 *
 * The point of this file existing today is:
 *   1. HybridRTManager's `case HardwareMetalRT` can call a real MetalRayTracingSystem
 *      object instead of going straight to SDFGI, so when any trace is implemented
 *      the wiring is already correct.
 *   2. The PIMPL structure shows exactly what Metal resources need to be
 *      created for a full implementation (AS heap, intersection tables,
 *      compute pipelines, output UAVs).
 *   3. The capability detection in MetalDevice::PopulateCapabilities still
 *      reports `HardwareMetalRT` when the GPU supports it; this system is
 *      the object that will host the real work.
 *
 * The one-shot warning log lets us see, from the engine console, that the
 * Metal RT path was selected but hasn't been implemented — which is far
 * better than the silent SDFGI fall-through we had before.
 */

#include "MetalRayTracing.h"

#ifdef SPARK_PLATFORM_MACOS

#include "MetalDevice.h"
#include "../../../Utils/Logger.h"
#include "../../../Utils/SparkConsole.h"

#import <Metal/Metal.h>

#include <atomic>

namespace Spark::RHI::Metal
{
    namespace
    {
        // Emit each "not implemented" warning exactly once per process so the
        // log is informative without spamming every frame.
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
                                   " not yet implemented — falling back to SDFGI");
            }
        }
    } // namespace

    struct MetalRayTracingSystem::Impl
    {
        MetalDevice* device = nullptr;
        id<MTLDevice> mtlDevice = nil;
        id<MTLCommandQueue> commandQueue = nil;

        // BLAS table — each entry holds the acceleration structure plus the
        // source geometry desc so UpdateBLAS can rebuild in place. The
        // `mtlAS` slot is nil today; CreateBLAS reserves an index so the
        // caller can proceed as if the build had happened.
        struct BLASEntry
        {
            id<MTLAccelerationStructure> mtlAS = nil;
            BLASGeometry geom{};
            bool live = false;
        };
        std::vector<BLASEntry> blas;

        id<MTLAccelerationStructure> tlas = nil;
        uint32_t tlasInstanceCount = 0;

        // Future: per-effect compute pipelines.
        //   id<MTLComputePipelineState> reflectionsPSO = nil;
        //   id<MTLComputePipelineState> shadowsPSO = nil;
        //   id<MTLComputePipelineState> aoPSO = nil;
        //   id<MTLComputePipelineState> giPSO = nil;

        bool piplinesReady = false;
    };

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

        // Acceleration structures + intersection pipelines are the next step.
        // Until they exist the system is "not available" from the HybridRT
        // manager's perspective so it falls back to SDFGI.
        m_impl->piplinesReady = false;
        m_available = false;

        SPARK_LOG_INFO(Spark::LogCategory::Graphics,
                       "MetalRayTracing: device supports ray tracing — scaffold registered "
                       "(trace pipelines not yet implemented)");
        return false;
    }

    void MetalRayTracingSystem::Shutdown()
    {
        if (!m_impl)
            return;

        for (auto& e : m_impl->blas)
        {
            e.mtlAS = nil;
            e.live = false;
        }
        m_impl->blas.clear();
        m_impl->tlas = nil;
        m_impl->tlasInstanceCount = 0;
        m_impl->piplinesReady = false;
        m_impl->mtlDevice = nil;
        m_impl->commandQueue = nil;
        m_impl->device = nullptr;
        m_available = false;
    }

    uint32_t MetalRayTracingSystem::CreateBLAS(const BLASGeometry& geometry)
    {
        // Reserve a slot. The real implementation will build an
        // MTLPrimitiveAccelerationStructureDescriptor from the vertex/index
        // buffers and call `[device accelerationStructureWithDescriptor:]`.
        Impl::BLASEntry entry{};
        entry.geom = geometry;
        entry.live = true;
        m_impl->blas.push_back(entry);
        return static_cast<uint32_t>(m_impl->blas.size() - 1);
    }

    void MetalRayTracingSystem::UpdateBLAS(uint32_t blasIndex, const BLASGeometry& geometry)
    {
        if (blasIndex >= m_impl->blas.size())
            return;
        m_impl->blas[blasIndex].geom = geometry;
        m_impl->blas[blasIndex].live = true;
    }

    void MetalRayTracingSystem::DestroyBLAS(uint32_t blasIndex)
    {
        if (blasIndex >= m_impl->blas.size())
            return;
        m_impl->blas[blasIndex].mtlAS = nil;
        m_impl->blas[blasIndex].live = false;
    }

    void MetalRayTracingSystem::BuildTLAS(const std::vector<TLASInstance>& instances)
    {
        // Real implementation: create an MTLInstanceAccelerationStructureDescriptor,
        // populate an MTLBuffer with MTLAccelerationStructureInstanceDescriptor
        // entries (one per TLASInstance), then schedule a build on the
        // MTLAccelerationStructureCommandEncoder. For the scaffold we just
        // record the instance count so GetStatusString has something useful.
        m_impl->tlasInstanceCount = static_cast<uint32_t>(instances.size());
    }

    bool MetalRayTracingSystem::TraceReflections()
    {
        WarnOnce(g_warnedReflections, "TraceReflections");
        return false;
    }

    bool MetalRayTracingSystem::TraceShadows()
    {
        WarnOnce(g_warnedShadows, "TraceShadows");
        return false;
    }

    bool MetalRayTracingSystem::TraceAmbientOcclusion()
    {
        WarnOnce(g_warnedAO, "TraceAmbientOcclusion");
        return false;
    }

    bool MetalRayTracingSystem::TraceGlobalIllumination()
    {
        WarnOnce(g_warnedGI, "TraceGlobalIllumination");
        return false;
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
            return "MetalRT: scaffold only (trace pipelines pending)";
        return std::string("MetalRT: live (BLAS=") + std::to_string(m_impl->blas.size()) +
               ", TLAS instances=" + std::to_string(m_impl->tlasInstanceCount) + ")";
    }
} // namespace Spark::RHI::Metal

#endif // SPARK_PLATFORM_MACOS
