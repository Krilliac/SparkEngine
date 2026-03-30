#pragma once

/**
 * @file ProfileProperties.h
 * @brief Typed profile properties that auto-reset each frame
 *
 * ProfileProperties provides named counters and gauges for performance
 * telemetry.  Properties flagged with FrameReset are automatically zeroed
 * at the start of each frame.  In release builds the macros compile to
 * no-ops so there is zero overhead.
 */

#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Spark
{

    /// @brief Flags controlling profile property behaviour
    enum class ProfilePropertyFlags : uint8_t
    {
        None = 0,
        FrameReset = 1 << 0 ///< Reset to zero at the start of each frame
    };

    /// @brief Inline bitwise OR for ProfilePropertyFlags
    constexpr ProfilePropertyFlags operator|(ProfilePropertyFlags a, ProfilePropertyFlags b)
    {
        return static_cast<ProfilePropertyFlags>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
    }

    /// @brief Inline bitwise AND for ProfilePropertyFlags
    constexpr ProfilePropertyFlags operator&(ProfilePropertyFlags a, ProfilePropertyFlags b)
    {
        return static_cast<ProfilePropertyFlags>(static_cast<uint8_t>(a) & static_cast<uint8_t>(b));
    }

    /// @brief A single profile property entry
    struct ProfilePropertyEntry
    {
        std::string name;
        float value = 0.0f;
        ProfilePropertyFlags flags = ProfilePropertyFlags::None;
    };

    /// @brief Singleton registry of frame-resetting typed profile counters
    class ProfileProperties
    {
      public:
        /// @brief Get the singleton instance
        [[deprecated("Use EngineContext::Get()->GetSystem<ProfileProperties>() instead")]]
        static ProfileProperties& GetInstance();

        /// @brief Register a named property
        /// @param name Human-readable name
        /// @param flags Property flags (default: FrameReset)
        /// @return Property ID for use with Set/Add/Get methods
        uint32_t RegisterProperty(const std::string& name,
                                  ProfilePropertyFlags flags = ProfilePropertyFlags::FrameReset);

        /// @brief Set a property's value as float
        void SetFloat(uint32_t id, float value);

        /// @brief Add to a property's float value
        void AddFloat(uint32_t id, float delta);

        /// @brief Set a property's value as unsigned integer (stored in float)
        void SetUInt(uint32_t id, uint32_t value);

        /// @brief Add to a property's unsigned integer value
        void AddUInt(uint32_t id, uint32_t delta);

        /// @brief Read back a property as float
        float GetFloat(uint32_t id) const;

        /// @brief Read back a property as unsigned integer
        uint32_t GetUInt(uint32_t id) const;

        /// @brief Reset all properties with the FrameReset flag to zero
        void ResetFrameProperties();

        /// @brief Read-only access to all registered properties
        const std::vector<ProfilePropertyEntry>& GetAllProperties() const;

        /// @brief Console command: return a human-readable status string
        std::string Console_GetStatus() const;

        /// @brief Clear all registered properties
        void Shutdown();

      private:
        ProfileProperties() = default;
        ~ProfileProperties() = default;

        ProfileProperties(const ProfileProperties&) = delete;
        ProfileProperties& operator=(const ProfileProperties&) = delete;

        std::vector<ProfilePropertyEntry> m_properties;
    };

    // ---------------------------------------------------------------------------
    // Macros — compile to no-ops in release builds
    // ---------------------------------------------------------------------------

#ifndef NDEBUG

/// @brief Register a profile property (debug only). Evaluates to the property ID.
#define SPARK_PROFILE_PROPERTY(name, flags) ::Spark::ProfileProperties::GetInstance().RegisterProperty(name, flags)

/// @brief Set a float profile property (debug only)
#define SPARK_PROFILE_SET_F32(id, val) ::Spark::ProfileProperties::GetInstance().SetFloat(id, val)

/// @brief Add to a uint32 profile property (debug only)
#define SPARK_PROFILE_ADD_U32(id, val) ::Spark::ProfileProperties::GetInstance().AddUInt(id, val)

#else

#define SPARK_PROFILE_PROPERTY(name, flags) 0
#define SPARK_PROFILE_SET_F32(id, val) ((void)0)
#define SPARK_PROFILE_ADD_U32(id, val) ((void)0)

#endif

} // namespace Spark
