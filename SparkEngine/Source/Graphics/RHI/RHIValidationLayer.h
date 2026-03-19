/**
 * @file RHIValidationLayer.h
 * @brief Debug-only validation decorator for RHI resource and state tracking
 * @author Spark Engine Team
 * @date 2026
 *
 * Wraps RHI calls to check resource states, binding correctness, and
 * format compatibility at runtime. All validation logic compiles to no-ops
 * in release builds via NDEBUG guards.
 *
 * @see RHI, RHIDevice, RHIResources
 */

#pragma once

#include "../../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Lifecycle state of a tracked RHI resource.
     */
    enum class ResourceState : uint8_t
    {
        Unknown = 0,
        Created,
        Bound,
        Released
    };

    /**
     * @brief A single validation error recorded by the validation layer.
     */
    struct ValidationError
    {
        std::string message;
        std::string context;
        uint32_t errorCode = 0;
    };

#ifndef NDEBUG

    /**
     * @brief Internal tracking info for a single resource.
     */
    struct TrackedResourceInfo
    {
        std::string name;
        std::string type;
        ResourceState state = ResourceState::Unknown;
    };

    /**
     * @brief Debug-only validation layer for RHI calls.
     *
     * Tracks resource lifecycles, validates bindings, and records errors
     * when incorrect API usage is detected. Compiles to an empty shell
     * in release builds.
     *
     * ## Usage
     * @code
     *   auto& validator = RHIValidationLayer::GetInstance();
     *   validator.Initialize();
     *   validator.TrackResource(handle, "MyTexture", "Texture2D");
     *   validator.SetResourceState(handle, ResourceState::Created);
     *
     *   if (!validator.ValidateBinding(shaderHandle, handle, 0))
     *   {
     *       // Binding validation failed — check GetErrors()
     *   }
     * @endcode
     */
    class RHIValidationLayer
    {
      public:
        /// @brief Get the singleton instance.
        static RHIValidationLayer& GetInstance()
        {
            static RHIValidationLayer instance;
            return instance;
        }

        RHIValidationLayer(const RHIValidationLayer&) = delete;
        RHIValidationLayer& operator=(const RHIValidationLayer&) = delete;

        /// @brief Initialize the validation layer.
        bool Initialize();

        /**
         * @brief Begin tracking a resource by handle.
         * @param handle Unique resource handle.
         * @param name Debug name for the resource.
         * @param type Resource type string (e.g., "Texture2D", "Buffer").
         */
        void TrackResource(uint64_t handle, const std::string& name, const std::string& type);

        /**
         * @brief Update the lifecycle state of a tracked resource.
         * @param handle Resource handle.
         * @param state New state.
         */
        void SetResourceState(uint64_t handle, ResourceState state);

        /**
         * @brief Query the current state of a tracked resource.
         * @param handle Resource handle.
         * @return Current state, or Unknown if not tracked.
         */
        ResourceState GetResourceState(uint64_t handle) const;

        /**
         * @brief Validate that a resource can be bound to a shader slot.
         * @param shaderHandle Handle of the shader program.
         * @param resourceHandle Handle of the resource to bind.
         * @param slot Binding slot index.
         * @return True if the binding is valid.
         */
        bool ValidateBinding(uint64_t shaderHandle, uint64_t resourceHandle, uint32_t slot);

        /**
         * @brief Validate draw call parameters.
         * @param vertexCount Number of vertices.
         * @param indexCount Number of indices (0 for non-indexed draws).
         * @return True if parameters are valid.
         */
        bool ValidateDrawCall(uint32_t vertexCount, uint32_t indexCount);

        /**
         * @brief Validate render target dimensions and state.
         * @param rtHandle Render target resource handle.
         * @param width Expected width.
         * @param height Expected height.
         * @return True if the render target is valid.
         */
        bool ValidateRenderTarget(uint64_t rtHandle, uint32_t width, uint32_t height);

        /// @brief Get all recorded validation errors.
        const std::vector<ValidationError>& GetErrors() const;

        /// @brief Clear all recorded validation errors.
        void ClearErrors();

        /// @brief Number of resources currently being tracked.
        uint32_t GetTrackedResourceCount() const;

        /// @brief Console status string for debugging.
        std::string Console_GetStatus() const;

        /// @brief Release all tracking data.
        void Shutdown();

      private:
        RHIValidationLayer() = default;

        /// @brief Record a validation error.
        void RecordError(uint32_t code, const std::string& message, const std::string& context);

        bool m_initialized = false;
        std::unordered_map<uint64_t, TrackedResourceInfo> m_trackedResources;
        std::vector<ValidationError> m_errors;
    };

#else // NDEBUG — Release builds: all methods are no-ops

    /**
     * @brief Release-build stub of RHIValidationLayer (all methods are no-ops).
     */
    class RHIValidationLayer
    {
      public:
        static RHIValidationLayer& GetInstance()
        {
            static RHIValidationLayer instance;
            return instance;
        }

        RHIValidationLayer(const RHIValidationLayer&) = delete;
        RHIValidationLayer& operator=(const RHIValidationLayer&) = delete;

        bool Initialize() { return true; }
        void TrackResource(uint64_t, const std::string&, const std::string&) {}
        void SetResourceState(uint64_t, ResourceState) {}
        ResourceState GetResourceState(uint64_t) const { return ResourceState::Unknown; }
        bool ValidateBinding(uint64_t, uint64_t, uint32_t) { return true; }
        bool ValidateDrawCall(uint32_t, uint32_t) { return true; }
        bool ValidateRenderTarget(uint64_t, uint32_t, uint32_t) { return true; }
        const std::vector<ValidationError>& GetErrors() const { return m_empty; }
        void ClearErrors() {}
        uint32_t GetTrackedResourceCount() const { return 0; }
        std::string Console_GetStatus() const { return "RHIValidationLayer: Disabled (Release build)\n"; }
        void Shutdown() {}

      private:
        RHIValidationLayer() = default;
        std::vector<ValidationError> m_empty;
    };

#endif // NDEBUG

} // namespace Spark::Graphics
