/**
 * @file RHIValidationLayer.cpp
 * @brief Implementation of the debug-only RHI validation layer
 * @author Spark Engine Team
 * @date 2026
 */

#include "RHIValidationLayer.h"

#include <format>

// All validation logic is compiled out in release builds
#ifndef NDEBUG

namespace Spark::Graphics
{

    bool RHIValidationLayer::Initialize()
    {
        if (m_initialized)
        {
            Shutdown();
        }

        m_trackedResources.clear();
        m_errors.clear();
        m_initialized = true;

        return true;
    }

    void RHIValidationLayer::TrackResource(uint64_t handle, const std::string& name, const std::string& type)
    {
        if (!m_initialized)
        {
            return;
        }

        if (m_trackedResources.contains(handle))
        {
            RecordError(
                1001,
                std::format("Resource handle {} already tracked as '{}'", handle, m_trackedResources[handle].name),
                "TrackResource");
            return;
        }

        TrackedResourceInfo info;
        info.name = name;
        info.type = type;
        info.state = ResourceState::Created;

        m_trackedResources[handle] = std::move(info);
    }

    void RHIValidationLayer::SetResourceState(uint64_t handle, ResourceState state)
    {
        if (!m_initialized)
        {
            return;
        }

        auto it = m_trackedResources.find(handle);
        if (it == m_trackedResources.end())
        {
            RecordError(1002, std::format("Attempting to set state on untracked resource {}", handle),
                        "SetResourceState");
            return;
        }

        // Validate state transitions
        auto& info = it->second;
        if (info.state == ResourceState::Released && state != ResourceState::Released)
        {
            RecordError(1003,
                        std::format("Resource '{}' ({}) is Released; cannot transition to state {}", info.name, handle,
                                    static_cast<int>(state)),
                        "SetResourceState");
            return;
        }

        info.state = state;
    }

    ResourceState RHIValidationLayer::GetResourceState(uint64_t handle) const
    {
        auto it = m_trackedResources.find(handle);
        if (it == m_trackedResources.end())
        {
            return ResourceState::Unknown;
        }
        return it->second.state;
    }

    bool RHIValidationLayer::ValidateBinding(uint64_t shaderHandle, uint64_t resourceHandle, uint32_t slot)
    {
        if (!m_initialized)
        {
            return true;
        }

        bool valid = true;

        // Validate shader exists and is in a usable state
        auto shaderIt = m_trackedResources.find(shaderHandle);
        if (shaderIt == m_trackedResources.end())
        {
            RecordError(2001, std::format("Shader handle {} is not tracked", shaderHandle), "ValidateBinding");
            valid = false;
        }
        else if (shaderIt->second.state == ResourceState::Released)
        {
            RecordError(2002, std::format("Shader '{}' ({}) has been released", shaderIt->second.name, shaderHandle),
                        "ValidateBinding");
            valid = false;
        }

        // Validate resource exists and is in a bindable state
        auto resIt = m_trackedResources.find(resourceHandle);
        if (resIt == m_trackedResources.end())
        {
            RecordError(2003, std::format("Resource handle {} is not tracked (slot {})", resourceHandle, slot),
                        "ValidateBinding");
            valid = false;
        }
        else if (resIt->second.state == ResourceState::Released)
        {
            RecordError(
                2004,
                std::format("Resource '{}' ({}) has been released (slot {})", resIt->second.name, resourceHandle, slot),
                "ValidateBinding");
            valid = false;
        }
        else if (resIt->second.state == ResourceState::Unknown)
        {
            RecordError(2005,
                        std::format("Resource '{}' ({}) is in Unknown state (slot {})", resIt->second.name,
                                    resourceHandle, slot),
                        "ValidateBinding");
            valid = false;
        }

        // Validate slot range (D3D11 max slots = 128)
        constexpr uint32_t MAX_BINDING_SLOTS = 128;
        if (slot >= MAX_BINDING_SLOTS)
        {
            RecordError(2006, std::format("Binding slot {} exceeds maximum ({})", slot, MAX_BINDING_SLOTS),
                        "ValidateBinding");
            valid = false;
        }

        return valid;
    }

    bool RHIValidationLayer::ValidateDrawCall(uint32_t vertexCount, uint32_t indexCount)
    {
        if (!m_initialized)
        {
            return true;
        }

        bool valid = true;

        if (vertexCount == 0)
        {
            RecordError(3001, "Draw call with 0 vertices", "ValidateDrawCall");
            valid = false;
        }

        // Very large draw calls are likely errors
        constexpr uint32_t SUSPICIOUS_VERTEX_COUNT = 10'000'000;
        if (vertexCount > SUSPICIOUS_VERTEX_COUNT)
        {
            RecordError(3002, std::format("Suspiciously large vertex count: {}", vertexCount), "ValidateDrawCall");
            // Warning only — don't fail validation
        }

        if (indexCount > 0 && indexCount > vertexCount * 6)
        {
            RecordError(3003,
                        std::format("Index count ({}) is disproportionately large relative to "
                                    "vertex count ({})",
                                    indexCount, vertexCount),
                        "ValidateDrawCall");
            // Warning only
        }

        return valid;
    }

    bool RHIValidationLayer::ValidateRenderTarget(uint64_t rtHandle, uint32_t width, uint32_t height)
    {
        if (!m_initialized)
        {
            return true;
        }

        bool valid = true;

        auto it = m_trackedResources.find(rtHandle);
        if (it == m_trackedResources.end())
        {
            RecordError(4001, std::format("Render target handle {} is not tracked", rtHandle), "ValidateRenderTarget");
            return false;
        }

        if (it->second.state == ResourceState::Released)
        {
            RecordError(4002, std::format("Render target '{}' ({}) has been released", it->second.name, rtHandle),
                        "ValidateRenderTarget");
            valid = false;
        }

        if (width == 0 || height == 0)
        {
            RecordError(4003, std::format("Render target dimensions are zero: {}x{}", width, height),
                        "ValidateRenderTarget");
            valid = false;
        }

        constexpr uint32_t MAX_RT_DIMENSION = 16384;
        if (width > MAX_RT_DIMENSION || height > MAX_RT_DIMENSION)
        {
            RecordError(
                4004,
                std::format("Render target dimensions exceed maximum: {}x{} (max {})", width, height, MAX_RT_DIMENSION),
                "ValidateRenderTarget");
            valid = false;
        }

        return valid;
    }

    const std::vector<ValidationError>& RHIValidationLayer::GetErrors() const
    {
        return m_errors;
    }

    void RHIValidationLayer::ClearErrors()
    {
        m_errors.clear();
    }

    uint32_t RHIValidationLayer::GetTrackedResourceCount() const
    {
        return static_cast<uint32_t>(m_trackedResources.size());
    }

    std::string RHIValidationLayer::Console_GetStatus() const
    {
        if (!m_initialized)
        {
            return "RHIValidationLayer: Not initialized\n";
        }

        uint32_t created = 0;
        uint32_t bound = 0;
        uint32_t released = 0;
        uint32_t unknown = 0;

        for (const auto& [handle, info] : m_trackedResources)
        {
            switch (info.state)
            {
            case ResourceState::Created:
                ++created;
                break;
            case ResourceState::Bound:
                ++bound;
                break;
            case ResourceState::Released:
                ++released;
                break;
            case ResourceState::Unknown:
                ++unknown;
                break;
            }
        }

        std::string status = "RHIValidationLayer (Debug):\n";
        status += std::format("  Tracked resources: {}\n", m_trackedResources.size());
        status += std::format("    Created:  {}\n", created);
        status += std::format("    Bound:    {}\n", bound);
        status += std::format("    Released: {}\n", released);
        status += std::format("    Unknown:  {}\n", unknown);
        status += std::format("  Recorded errors: {}\n", m_errors.size());

        return status;
    }

    void RHIValidationLayer::Shutdown()
    {
        m_trackedResources.clear();
        m_errors.clear();
        m_initialized = false;
    }

    void RHIValidationLayer::RecordError(uint32_t code, const std::string& message, const std::string& context)
    {
        ValidationError error;
        error.message = message;
        error.context = context;
        error.errorCode = code;
        m_errors.push_back(std::move(error));
    }

} // namespace Spark::Graphics

#endif // NDEBUG
