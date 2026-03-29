/**
 * @file MaterialPropertyHandle.h
 * @brief Handle-based material uniform access eliminating runtime string lookups
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a compact encoded handle for material properties that can be
 * resolved once at load time and used for O(1) access during rendering.
 * The MaterialPropertyRegistry singleton maps string names to handles.
 *
 * @see MaterialSystem, Shader
 */

#pragma once

#include "../Core/Platform.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Graphics
{

    /**
     * @brief Type of a material property value.
     */
    enum class PropertyType : uint8_t
    {
        Float = 0,
        Float2,
        Float3,
        Float4,
        Int,
        Matrix4x4,
        Texture,
        Count
    };

    /**
     * @brief Compact handle encoding binding index, type, and byte offset.
     *
     * Layout (32 bits):
     *   - bits [0..15]:  byte offset within the constant buffer (16 bits)
     *   - bits [16..21]: property type (6 bits)
     *   - bits [22..31]: binding index (10 bits)
     */
    struct MaterialPropertyHandle
    {
        uint32_t encoded = 0xFFFFFFFF;

        /// @brief Extract the binding index (bits 22..31).
        uint16_t GetBindingIndex() const { return static_cast<uint16_t>((encoded >> 22) & 0x3FF); }

        /// @brief Extract the property type (bits 16..21).
        uint8_t GetType() const { return static_cast<uint8_t>((encoded >> 16) & 0x3F); }

        /// @brief Extract the byte offset (bits 0..15).
        uint16_t GetOffset() const { return static_cast<uint16_t>(encoded & 0xFFFF); }

        /// @brief Check whether this handle refers to a valid property.
        bool IsValid() const { return encoded != 0xFFFFFFFF; }

        /// @brief Create an invalid handle sentinel.
        static MaterialPropertyHandle Invalid() { return MaterialPropertyHandle{0xFFFFFFFF}; }

        bool operator==(const MaterialPropertyHandle& other) const = default;
    };

    /**
     * @brief Singleton registry mapping property names to encoded handles.
     *
     * Properties are registered once (typically at material or shader load time)
     * and looked up by handle during rendering for zero-cost access.
     *
     * ## Usage
     * @code
     *   auto& reg = MaterialPropertyRegistry::GetInstance();
     *   auto handle = reg.RegisterProperty("baseColor", PropertyType::Float4, 0, 0);
     *   auto found = reg.FindProperty("baseColor");
     *   assert(found.IsValid());
     * @endcode
     */
    class MaterialPropertyRegistry
    {
      public:
        /// @brief Get the singleton instance.
        [[deprecated("Use EngineContext::Get()->GetSystem<MaterialPropertyRegistry>() instead")]]
        static MaterialPropertyRegistry& GetInstance()
        {
            static MaterialPropertyRegistry instance;
            return instance;
        }

        MaterialPropertyRegistry(const MaterialPropertyRegistry&) = delete;
        MaterialPropertyRegistry& operator=(const MaterialPropertyRegistry&) = delete;

        /**
         * @brief Register a new material property.
         * @param name Human-readable property name.
         * @param type Value type of the property.
         * @param bindingIndex Constant buffer binding index (0..1023).
         * @param offset Byte offset within the constant buffer (0..65535).
         * @return Encoded handle for the property, or Invalid() on failure.
         */
        MaterialPropertyHandle RegisterProperty(const std::string& name, PropertyType type, uint16_t bindingIndex,
                                                uint16_t offset);

        /**
         * @brief Look up a property handle by name (O(1) hash lookup).
         * @param name Property name.
         * @return The handle, or Invalid() if not registered.
         */
        MaterialPropertyHandle FindProperty(const std::string& name) const;

        /**
         * @brief Get the name of a property from its handle.
         * @param handle Encoded property handle.
         * @return Property name, or empty string if not found.
         */
        const std::string& GetPropertyName(MaterialPropertyHandle handle) const;

        /**
         * @brief Get the type of a property from its handle.
         * @param handle Encoded property handle.
         * @return Property type.
         */
        PropertyType GetPropertyType(MaterialPropertyHandle handle) const;

        /// @brief Number of registered properties.
        uint32_t GetRegisteredPropertyCount() const;

        /// @brief Console status string for debugging.
        std::string Console_GetStatus() const;

        /// @brief Clear all registered properties.
        void Shutdown();

      private:
        MaterialPropertyRegistry() = default;

        /// Name -> Handle map for O(1) lookup
        std::unordered_map<std::string, MaterialPropertyHandle> m_nameToHandle;

        /// Handle encoded value -> Name reverse map
        std::unordered_map<uint32_t, std::string> m_handleToName;

        /// Handle encoded value -> PropertyType map
        std::unordered_map<uint32_t, PropertyType> m_handleToType;
    };

} // namespace Spark::Graphics
