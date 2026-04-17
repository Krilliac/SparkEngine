/**
 * @file RHIDeviceBase.h
 * @brief Shared base class for concrete IRHIDevice implementations.
 *
 * Holds the common device-level state that every backend previously duplicated:
 *   - `RHIDeviceCapabilities` and its inline getter
 *   - `RHIStatistics` counters, inline getter, and `ResetStatistics()` body
 *   - The per-device `TransientBufferAllocator` (identical 4 MiB / 2 MiB config
 *     across D3D11, D3D12, Vulkan, and OpenGL)
 *
 * Backends inherit from `RHIDeviceBase` instead of `IRHIDevice` directly.
 * Lifecycle methods that only differ in auxiliary work (fence pacing,
 * deletion queues, etc.) stay on the concrete backend; they call
 * `m_transientBuffers.BeginFrame/EndFrame(this)` and `ResetStatistics()`
 * from the base.
 */

#pragma once

#include "RHIDevice.h"
#include "TransientBufferAllocator.h"

namespace Spark::RHI
{

    /**
     * @brief Common state and boilerplate for RHI device implementations.
     */
    class RHIDeviceBase : public IRHIDevice
    {
      public:
        const RHIDeviceCapabilities& GetCapabilities() const final { return m_capabilities; }
        const RHIStatistics& GetStatistics() const final { return m_statistics; }
        void ResetStatistics() final { m_statistics = {}; }

      protected:
        RHIDeviceCapabilities m_capabilities;
        RHIStatistics m_statistics;

        // Per-frame ring allocator for transient uploads (dynamic vertex/index
        // buffers, UI, debug draw). 4 MiB vertex arena / 2 MiB index arena —
        // sized to cover the common imgui + debug-draw workload without
        // spilling. Backends pump it via `m_transientBuffers.BeginFrame(this)`
        // / `EndFrame(this)` inside their own BeginFrame/EndFrame overrides.
        TransientBufferAllocator m_transientBuffers{4 * 1024 * 1024, 2 * 1024 * 1024};
    };

} // namespace Spark::RHI
