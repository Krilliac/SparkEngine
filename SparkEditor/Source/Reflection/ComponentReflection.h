/**
 * @file ComponentReflection.h
 * @brief Advanced component system with reflection for automatic property editing
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements a sophisticated reflection system that allows automatic
 * generation of property editors for components, enabling dynamic inspection
 * and modification of component properties at runtime.
 */

#pragma once

#include "../SceneSystem/SceneFile.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <typeinfo>
#include <memory>
#include <variant>

namespace SparkEditor
{

    /**
 * @brief Variant type for holding different property values
 */
    using PropertyValue = std::variant<bool, int, float, double, std::string, XMFLOAT2, XMFLOAT3, XMFLOAT4, XMMATRIX>;

    /**
 * @brief Property data types
 */
    enum class PropertyType
    {
        BOOL,
        INT,
        FLOAT,
        DOUBLE,
        STRING,
        VECTOR2,
        VECTOR3,
        VECTOR4,
        MATRIX,
        COLOR,
        TEXTURE,
        MESH,
        MATERIAL,
        AUDIO_CLIP,
        ENUM,
        CUSTOM
    };

    /**
 * @brief Property metadata for UI generation
 */
    struct PropertyMetadata
    {
        std::string name;                    ///< Property display name
        std::string description;             ///< Property description/tooltip
        PropertyType type;                   ///< Property data type
        bool isReadOnly = false;             ///< Whether property is read-only
        bool isVisible = true;               ///< Whether property is visible in UI
        PropertyValue defaultValue;          ///< Default property value
        PropertyValue minValue;              ///< Minimum value (for numeric types)
        PropertyValue maxValue;              ///< Maximum value (for numeric types)
        float step = 0.1f;                   ///< Step size for numeric inputs
        std::vector<std::string> enumValues; ///< Enum value names
        std::string category = "General";    ///< Property category for grouping
        int displayOrder = 0;                ///< Display order in UI

        // Validation and formatting
        std::function<bool(const PropertyValue&)> validator;        ///< Value validation function
        std::function<std::string(const PropertyValue&)> formatter; ///< Value formatting function
        std::function<void(const PropertyValue&)> changeCallback;   ///< Value change callback
    };

    /**
 * @brief Component type information
 */
    struct ComponentTypeInfo
    {
        std::string typeName;                    ///< Component type name
        std::string displayName;                 ///< Display name for UI
        std::string description;                 ///< Component description
        std::string category = "General";        ///< Component category
        std::string iconPath;                    ///< Icon file path
        size_t sizeInBytes = 0;                  ///< Component size in bytes
        bool allowMultiple = false;              ///< Allow multiple instances per object
        std::vector<ComponentType> dependencies; ///< Required components
        std::vector<ComponentType> conflicts;    ///< Conflicting components

        // Component lifecycle functions
        std::function<void*(void)> constructor;           ///< Component constructor
        std::function<void(void*)> destructor;            ///< Component destructor
        std::function<void*(const void*)> copier;         ///< Component copy function
        std::function<void(void*, const void*)> assigner; ///< Component assignment

        // Serialization functions
        std::function<void(const void*, std::vector<uint8_t>&)> serializer;   ///< Serialize to bytes
        std::function<bool(void*, const std::vector<uint8_t>&)> deserializer; ///< Deserialize from bytes

        // Property access
        std::vector<PropertyMetadata> properties;            ///< Component properties
        std::unordered_map<std::string, size_t> propertyMap; ///< Property name to index map
    };

    /**
 * @brief Advanced component reflection system
 * 
 * Provides comprehensive reflection capabilities for components, enabling:
 * - Automatic property editor generation
 * - Runtime property inspection and modification
 * - Component serialization and deserialization
 * - Type-safe property access
 * - Validation and constraint checking
 * - Undo/redo integration
 * - Custom property editors and formatters
 * 
 * Inspired by Unreal's reflection system and Unity's SerializedProperty system.
 */
    class ComponentReflection
    {
      public:
        /**
     * @brief Get singleton instance
     * @return Reference to singleton instance
     */
        static ComponentReflection& GetInstance();

        /**
     * @brief Register a component type with reflection data
     * @tparam T Component type to register
     * @param typeInfo Component type information
     */
        template <typename T> void RegisterComponentType(const ComponentTypeInfo& typeInfo);

        /**
     * @brief Register a component type with automatic reflection
     * @tparam T Component type to register
     * @param displayName Display name for the component
     * @param category Component category
     */
        template <typename T>
        void RegisterComponentType(const std::string& displayName, const std::string& category = "General");

        /**
     * @brief Get component type information
     * @param type Component type
     * @return Pointer to type info, or nullptr if not found
     */
        const ComponentTypeInfo* GetComponentTypeInfo(ComponentType type) const;

        /**
     * @brief Get all registered component types
     * @return Vector of all registered component types
     */
        std::vector<ComponentType> GetRegisteredComponentTypes() const;

        /**
     * @brief Get component types by category
     * @param category Category name
     * @return Vector of component types in the category
     */
        std::vector<ComponentType> GetComponentTypesByCategory(const std::string& category) const;

        /**
     * @brief Get all component categories
     * @return Vector of category names
     */
        std::vector<std::string> GetComponentCategories() const;

        /**
     * @brief Create a new component instance
     * @param type Component type to create
     * @return Pointer to new component data, or nullptr if failed
     */
        void* CreateComponent(ComponentType type) const;

        /**
     * @brief Destroy a component instance
     * @param type Component type
     * @param componentData Pointer to component data
     */
        void DestroyComponent(ComponentType type, void* componentData) const;

        /**
     * @brief Copy a component instance
     * @param type Component type
     * @param sourceData Source component data
     * @return Pointer to copied component data
     */
        void* CopyComponent(ComponentType type, const void* sourceData) const;

        /**
     * @brief Get property value from component
     * @param type Component type
     * @param componentData Component data pointer
     * @param propertyName Property name
     * @return Property value, or empty variant if not found
     */
        PropertyValue GetPropertyValue(ComponentType type, const void* componentData,
                                       const std::string& propertyName) const;

        /**
     * @brief Set property value in component
     * @param type Component type
     * @param componentData Component data pointer
     * @param propertyName Property name
     * @param value New property value
     * @return true if property was set successfully
     */
        bool SetPropertyValue(ComponentType type, void* componentData, const std::string& propertyName,
                              const PropertyValue& value) const;

        /**
     * @brief Validate property value
     * @param type Component type
     * @param propertyName Property name
     * @param value Value to validate
     * @return true if value is valid
     */
        bool ValidatePropertyValue(ComponentType type, const std::string& propertyName,
                                   const PropertyValue& value) const;

        /**
     * @brief Serialize component to byte array
     * @param type Component type
     * @param componentData Component data pointer
     * @param outData Output byte array
     * @return true if serialization succeeded
     */
        bool SerializeComponent(ComponentType type, const void* componentData, std::vector<uint8_t>& outData) const;

        /**
     * @brief Deserialize component from byte array
     * @param type Component type
     * @param componentData Component data pointer
     * @param data Input byte array
     * @return true if deserialization succeeded
     */
        bool DeserializeComponent(ComponentType type, void* componentData, const std::vector<uint8_t>& data) const;

        /**
     * @brief Check if component type can be added to object
     * @param objectComponents Current object components
     * @param newComponentType Type to add
     * @param outReason Output reason if not allowed
     * @return true if component can be added
     */
        bool CanAddComponent(const std::vector<ComponentType>& objectComponents, ComponentType newComponentType,
                             std::string& outReason) const;

        /**
     * @brief Get required components for a component type
     * @param type Component type
     * @return Vector of required component types
     */
        std::vector<ComponentType> GetRequiredComponents(ComponentType type) const;

        /**
     * @brief Get conflicting components for a component type
     * @param type Component type
     * @return Vector of conflicting component types
     */
        std::vector<ComponentType> GetConflictingComponents(ComponentType type) const;

        /**
     * @brief Convert PropertyValue to string for display
     * @param value Property value
     * @return String representation
     */
        static std::string PropertyValueToString(const PropertyValue& value);

        /**
     * @brief Parse PropertyValue from string
     * @param str String representation
     * @param type Expected property type
     * @return Parsed property value
     */
        static PropertyValue PropertyValueFromString(const std::string& str, PropertyType type);

        /**
     * @brief Get property type from PropertyValue
     * @param value Property value
     * @return Property type
     */
        static PropertyType GetPropertyType(const PropertyValue& value);

      private:
        /**
     * @brief Private constructor for singleton
     */
        ComponentReflection() = default;

        /**
     * @brief Register built-in component types
     */
        void RegisterBuiltInComponents();

        /**
     * @brief Create automatic reflection data for a component
     * @tparam T Component type
     * @param displayName Display name
     * @param category Category name
     * @return Component type information
     */
        template <typename T>
        ComponentTypeInfo CreateAutoReflection(const std::string& displayName, const std::string& category);

      private:
        std::unordered_map<ComponentType, ComponentTypeInfo> m_componentTypes;          ///< Registered component types
        std::unordered_map<std::string, std::vector<ComponentType>> m_categorizedTypes; ///< Types by category

        static ComponentReflection* s_instance; ///< Singleton instance
    };

/**
 * @brief Property registration helper macros
 */
#define REFLECT_PROPERTY(ComponentType, PropertyName, DisplayName, Description, Category)                              \
    RegisterProperty<ComponentType>(#PropertyName, DisplayName, Description, Category, &ComponentType::PropertyName)

#define REFLECT_PROPERTY_RANGE(ComponentType, PropertyName, DisplayName, Description, Category, MinVal, MaxVal)        \
    RegisterPropertyWithRange<ComponentType>(#PropertyName, DisplayName, Description, Category,                        \
                                             &ComponentType::PropertyName, MinVal, MaxVal)

#define REFLECT_PROPERTY_ENUM(ComponentType, PropertyName, DisplayName, Description, Category, EnumValues)             \
    RegisterPropertyEnum<ComponentType>(#PropertyName, DisplayName, Description, Category,                             \
                                        &ComponentType::PropertyName, EnumValues)

    /**
 * @brief Component registration helper class
 * @tparam T Component type to register
 */
    template <typename T> class ComponentRegistrar
    {
      public:
        /**
     * @brief Constructor - automatically registers component
     * @param displayName Component display name
     * @param category Component category
     */
        ComponentRegistrar(const std::string& displayName, const std::string& category = "General")
        {
            ComponentReflection::GetInstance().RegisterComponentType<T>(displayName, category);
        }

        /**
     * @brief Register a property
     * @tparam PropertyType Type of the property
     * @param propertyName Property name
     * @param displayName Display name
     * @param description Property description
     * @param category Property category
     * @param memberPtr Pointer to member variable
     * @return Reference to this registrar for chaining
     */
        template <typename PropertyType>
        ComponentRegistrar& Property(const std::string& propertyName, const std::string& displayName,
                                     const std::string& description, const std::string& category,
                                     PropertyType T::*memberPtr)
        {
            // Implementation would register the property with reflection system
            return *this;
        }
    };

/**
 * @brief Helper macro for component registration
 */
#define REGISTER_COMPONENT(ComponentType, DisplayName, Category)                                                       \
    static ComponentRegistrar<ComponentType> g_##ComponentType##_registrar(DisplayName, Category)

    // Template implementations
    template <typename T> void ComponentReflection::RegisterComponentType(const ComponentTypeInfo& typeInfo)
    {
        ComponentType componentType = static_cast<ComponentType>(typeid(T).hash_code());
        m_componentTypes[componentType] = typeInfo;
        m_categorizedTypes[typeInfo.category].push_back(componentType);
    }

    template <typename T>
    void ComponentReflection::RegisterComponentType(const std::string& displayName, const std::string& category)
    {
        ComponentTypeInfo typeInfo = CreateAutoReflection<T>(displayName, category);
        RegisterComponentType<T>(typeInfo);
    }

    template <typename T>
    ComponentTypeInfo ComponentReflection::CreateAutoReflection(const std::string& displayName,
                                                                const std::string& category)
    {
        ComponentTypeInfo info;
        info.typeName = typeid(T).name();
        info.displayName = displayName;
        info.category = category;
        info.sizeInBytes = sizeof(T);

        // Set up basic lifecycle functions
        info.constructor = []() { return new T(); };
        info.destructor = [](void* ptr) { delete static_cast<T*>(ptr); };
        info.copier = [](const void* src) { return new T(*static_cast<const T*>(src)); };
        info.assigner = [](void* dst, const void* src) { *static_cast<T*>(dst) = *static_cast<const T*>(src); };

        // Set up serialization functions
        info.serializer = [](const void* src, std::vector<uint8_t>& data)
        {
            const T* component = static_cast<const T*>(src);
            data.resize(sizeof(T));
            memcpy(data.data(), component, sizeof(T));
        };

        info.deserializer = [](void* dst, const std::vector<uint8_t>& data)
        {
            if (data.size() != sizeof(T))
                return false;
            memcpy(dst, data.data(), sizeof(T));
            return true;
        };

        return info;
    }

} // namespace SparkEditor