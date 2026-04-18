/**
 * @file MetalRayTracing.h
 * @brief Metal 2.4+ hardware ray-tracing backend for HybridRTManager.
 *
 * This is the scaffolding file for the Metal RT path. Metal's ray-tracing
 * API (`MTLAccelerationStructure`, `MTLIntersector`, ray query in compute
 * shaders) is only available on macOS 12 / iOS 15+ and Apple Silicon GPUs
 * that report `supportsRaytracing`. The capability is already detected in
 * `MetalDevice::PopulateCapabilities()` (→ `RayTracingBackend::HardwareMetalRT`),
 * but HybridRTManager currently falls straight through to SDFGI because no
 * actual Metal RT work has been implemented.
 *
 * This class defines the full surface that the HybridRT manager expects:
 *  - BLAS/TLAS construction from engine scene geometry
 *  - Per-effect trace dispatches (reflections, shadows, AO, GI)
 *  - Shader / pipeline / argument-buffer lifecycle
 *
 * Every method is currently a no-op stub that logs a one-shot warning and
 * returns a "not executed" result so callers can skip to the SDFGI fallback
 * without a crash. Replacing the stubs with real MTLAccelerationStructure /
 * MTLComputePipelineState work is tracked as the next Metal RT milestone.
 *
 * The header only declares the C++ interface; all Metal-specific types
 * (`id<MTLDevice>`, etc.) are kept inside the `.mm` file via a forward-
 * declared `Impl` pointer so this header is safe to include from non-
 * Objective-C++ translation units (HybridRTManager.cpp in particular).
 */

#pragma once

#include "../../../Core/Platform.h"

#ifdef SPARK_PLATFORM_MACOS

#include "../RHITypes.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::RHI::Metal
{
    class MetalDevice;

    /**
     * @brief Per-mesh geometry handed to the BLAS builder.
     *
     * Pointer memory must remain live until the Build call returns. The
     * descriptor intentionally mirrors DXR's BLASDesc so the scene-level
     * code paths can be shared once the Metal path is implemented.
     */
    struct BLASGeometry
    {
        std::string name;
        const void* vertexData = nullptr;
        uint32_t vertexCount = 0;
        uint32_t vertexStride = 0;
        const uint32_t* indexData = nullptr;
        uint32_t indexCount = 0;
        bool isOpaque = true;
        bool allowUpdate = false;
    };

    /**
     * @brief Per-instance TLAS entry (row-major 3x4 affine transform).
     */
    struct TLASInstance
    {
        uint32_t blasIndex = 0;
        float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        uint32_t instanceID = 0;
        uint32_t hitGroupIndex = 0;
        uint8_t instanceMask = 0xFF;
    };

    /**
     * @brief Which trace passes the HybridRT manager wants this frame.
     *        Mirrors the existing `RTEffect` flags in `RHITypes.h`.
     */
    enum class TracePass : uint32_t
    {
        None = 0,
        Reflections = 1u << 0,
        Shadows = 1u << 1,
        AmbientOcclusion = 1u << 2,
        GlobalIllumination = 1u << 3,
    };

    /**
     * @brief Metal hardware RT wrapper — one per MetalDevice.
     *
     * Ownership: HybridRTManager keeps a unique_ptr to this and drives it
     * each frame. Destruction releases all MTLAccelerationStructure /
     * MTLComputePipelineState resources via the opaque Impl pointer.
     *
     * Thread-safety: single-threaded (render thread). BLAS/TLAS build
     * calls must be issued before any trace call in the same frame.
     */
    class MetalRayTracingSystem
    {
      public:
        MetalRayTracingSystem();
        ~MetalRayTracingSystem();

        MetalRayTracingSystem(const MetalRayTracingSystem&) = delete;
        MetalRayTracingSystem& operator=(const MetalRayTracingSystem&) = delete;

        /**
         * @brief Initialize with an already-created MetalDevice.
         * @return true if the device reports `supportsRaytracing` AND the
         *         trace pipelines compiled. Currently returns false until
         *         the real implementation lands.
         */
        bool Initialize(MetalDevice* device);
        void Shutdown();

        /**
         * @brief Whether the system is live. When false, HybridRTManager
         *        falls through to SDFGI.
         */
        bool IsAvailable() const { return m_available; }

        /**
         * @brief Rebuild the bottom-level AS for a single mesh. Returns
         *        a stable index that TLASInstance::blasIndex references.
         *        Returns `UINT32_MAX` on failure.
         */
        uint32_t CreateBLAS(const BLASGeometry& geometry);
        void UpdateBLAS(uint32_t blasIndex, const BLASGeometry& geometry);
        void DestroyBLAS(uint32_t blasIndex);

        /**
         * @brief Rebuild the top-level AS from the provided instance list.
         *        Called once per frame before the trace passes.
         */
        void BuildTLAS(const std::vector<TLASInstance>& instances);

        /**
         * @brief Run one of the trace passes. Returns true if the pass
         *        actually dispatched; false means the caller should run
         *        the SDFGI fallback for this pass.
         *
         *        Today every overload returns false (scaffold only).
         */
        bool TraceReflections();
        bool TraceShadows();
        bool TraceAmbientOcclusion();
        bool TraceGlobalIllumination();

        /// @brief Aggregate: dispatch everything in `passes`. Returns the
        ///        set of passes that actually ran (may be a strict subset
        ///        when some shaders failed to compile).
        TracePass DispatchFrame(TracePass passes);

        /// @brief Human-readable status string for console output.
        std::string GetStatusString() const;

      private:
        struct Impl; // PIMPL so this header does not pull in Metal.
        std::unique_ptr<Impl> m_impl;
        bool m_available = false;
    };

    inline TracePass operator|(TracePass a, TracePass b)
    {
        return static_cast<TracePass>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline TracePass operator&(TracePass a, TracePass b)
    {
        return static_cast<TracePass>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    inline bool Any(TracePass p)
    {
        return static_cast<uint32_t>(p) != 0;
    }
} // namespace Spark::RHI::Metal

#endif // SPARK_PLATFORM_MACOS
