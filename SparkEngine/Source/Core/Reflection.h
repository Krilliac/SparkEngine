/**
 * @file Reflection.h
 * @brief Lightweight compile-time type reflection system (ezEngine-inspired)
 *
 * Provides runtime type metadata for C++ types across DLL boundaries.
 * Uses the same TypeId pattern as EngineContext for consistency.
 *
 * ### Features
 * - Register types with SPARK_REFLECT_TYPE() macro
 * - Register fields with SPARK_REFLECT_FIELD() macro
 * - Query all registered types and their fields at runtime
 * - Cross-DLL type discovery via static initializers
 * - No RTTI required (uses compile-time TypeId)
 *
 * ### Usage
 * ```cpp
 * // In header:
 * struct Transform {
 *     XMFLOAT3 position;
 *     XMFLOAT3 rotation;
 *     XMFLOAT3 scale;
 * };
 *
 * // In one .cpp file:
 * SPARK_REFLECT_TYPE(Transform)
 *     SPARK_REFLECT_FIELD(Transform, position, "Position")
 *     SPARK_REFLECT_FIELD(Transform, rotation, "Rotation")
 *     SPARK_REFLECT_FIELD(Transform, scale, "Scale")
 * SPARK_REFLECT_END()
 * ```
 *
 * @threadsafety Registration happens at static init time (single-threaded).
 *               Queries are read-only after startup.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// TypeId and GetTypeId are defined in EngineContext.h — include it rather than
// duplicating the definition, which causes "already defined" errors on MSVC.
#include "Core/EngineContext.h"

namespace Spark
{

    // ========================================================================
    // Field type enumeration
    // ========================================================================

    enum class FieldType : uint8_t
    {
        Unknown,
        Bool,
        Int,
        Float,
        Double,
        String,
        Vector3, ///< XMFLOAT3 or equivalent 3-float struct
        Vector4, ///< XMFLOAT4 or equivalent 4-float struct
        Enum,
        Custom ///< Opaque type (user handles serialization)
    };

    // ========================================================================
    // Field metadata
    // ========================================================================

    /**
     * @brief Runtime metadata for a single struct/class field.
     */
    struct FieldInfo
    {
        std::string name;      ///< Display name (e.g., "Position")
        std::string fieldName; ///< C++ member name (e.g., "position")
        FieldType type;        ///< Data type category
        size_t offset;         ///< Byte offset from struct start (via offsetof)
        size_t size;           ///< Size in bytes
        TypeId ownerType;      ///< TypeId of the owning struct
        float rangeMin = 0.0f; ///< Optional: minimum value hint for editor
        float rangeMax = 0.0f; ///< Optional: maximum value hint for editor
        bool hasRange = false; ///< Whether range constraints are set
        bool readOnly = false; ///< Whether the field is read-only in editor
    };

    // ========================================================================
    // Type metadata
    // ========================================================================

    /**
     * @brief Runtime metadata for a registered type.
     */
    struct TypeInfo
    {
        std::string name;          ///< Type name (e.g., "Transform")
        TypeId typeId = nullptr;   ///< Unique compile-time type identifier
        size_t size = 0;           ///< sizeof(T)
        size_t alignment = 0;      ///< alignof(T)
        TypeId baseType = nullptr; ///< Base class TypeId (nullptr if none)
        std::vector<FieldInfo> fields;

        /** @brief Find a field by C++ member name. Returns nullptr if not found. */
        const FieldInfo* FindField(std::string_view memberName) const
        {
            for (const auto& f : fields)
            {
                if (f.fieldName == memberName)
                    return &f;
            }
            return nullptr;
        }
    };

    // ========================================================================
    // Global type registry
    // ========================================================================

    /**
     * @brief Central registry of all reflected types.
     *
     * Types register themselves via static initializers (SPARK_REFLECT_TYPE macro).
     * After startup, the registry is read-only and can be queried from any thread.
     */
    class TypeRegistry
    {
      public:
        static TypeRegistry& Get()
        {
            static TypeRegistry instance;
            return instance;
        }

        /**
         * @brief Register a type with its metadata.
         * @return Reference to the stored TypeInfo for adding fields.
         */
        TypeInfo& RegisterType(TypeId id, const std::string& name, size_t size, size_t alignment,
                               TypeId baseType = nullptr)
        {
            auto& info = m_types[id];
            info.name = name;
            info.typeId = id;
            info.size = size;
            info.alignment = alignment;
            info.baseType = baseType;
            return info;
        }

        /** @brief Look up type metadata by TypeId. Returns nullptr if not registered. */
        const TypeInfo* FindType(TypeId id) const
        {
            auto it = m_types.find(id);
            return (it != m_types.end()) ? &it->second : nullptr;
        }

        /** @brief Look up type metadata by name. Returns nullptr if not registered. */
        const TypeInfo* FindTypeByName(std::string_view name) const
        {
            for (const auto& [id, info] : m_types)
            {
                if (info.name == name)
                    return &info;
            }
            return nullptr;
        }

        /** @brief Get all registered types. */
        const std::unordered_map<TypeId, TypeInfo>& GetAllTypes() const { return m_types; }

        /** @brief Get count of registered types. */
        size_t GetTypeCount() const { return m_types.size(); }

        /**
         * @brief Get all types derived from a given base type.
         * @tparam Base The base class to search for.
         * @return Vector of TypeInfo pointers for all derived types.
         */
        template <typename Base> std::vector<const TypeInfo*> GetDerivedTypes() const
        {
            TypeId baseId = GetTypeId<Base>();
            std::vector<const TypeInfo*> result;
            for (const auto& [id, info] : m_types)
            {
                if (info.baseType == baseId)
                    result.push_back(&info);
            }
            return result;
        }

      private:
        TypeRegistry() = default;
        std::unordered_map<TypeId, TypeInfo> m_types;
    };

    // ========================================================================
    // Auto-detection of FieldType from C++ types
    // ========================================================================

    template <typename T> constexpr FieldType DeduceFieldType()
    {
        if constexpr (std::is_same_v<T, bool>)
            return FieldType::Bool;
        else if constexpr (std::is_integral_v<T>)
            return FieldType::Int;
        else if constexpr (std::is_same_v<T, float>)
            return FieldType::Float;
        else if constexpr (std::is_same_v<T, double>)
            return FieldType::Double;
        else if constexpr (std::is_same_v<T, std::string>)
            return FieldType::String;
        else if constexpr (std::is_enum_v<T>)
            return FieldType::Enum;
        else
            return FieldType::Custom;
    }

    // ========================================================================
    // Component factory — dynamic add/has/remove by type name
    // ========================================================================

    /**
     * @brief Function signatures for runtime component operations on a World.
     *
     * These type-erased callbacks enable adding, checking, removing, and
     * getting raw pointers to components by string name — bridging the
     * gap between data-driven archetype files and the compile-time ECS API.
     */
    struct ComponentOps
    {
        using AddFn = void (*)(void* world, uint32_t entity);
        using HasFn = bool (*)(void* world, uint32_t entity);
        using RemoveFn = void (*)(void* world, uint32_t entity);
        using GetRawFn = void* (*)(void* world, uint32_t entity);

        AddFn add = nullptr;       ///< Add a default-constructed component.
        HasFn has = nullptr;       ///< Check if entity has the component.
        RemoveFn remove = nullptr; ///< Remove the component.
        GetRawFn getRaw = nullptr; ///< Get a raw pointer to the component data.
    };

    /**
     * @brief Applies a string value to a reflected field on a component instance.
     *
     * Supports Bool, Int, Float, Double, String, Vector3 (x,y,z), and Vector4.
     *
     * @param component  Raw pointer to the component struct.
     * @param field      Reflected field metadata.
     * @param value      String representation of the value to set.
     * @return true if the value was successfully parsed and written.
     */
    bool SetFieldFromString(void* component, const FieldInfo& field, const std::string& value);

    /**
     * @brief Reads a reflected field and returns its value as a string.
     *
     * @param component  Raw pointer to the component struct.
     * @param field      Reflected field metadata.
     * @return String representation, or empty string if type is unsupported.
     */
    std::string GetFieldAsString(const void* component, const FieldInfo& field);

    /**
     * @brief Central registry of component factory operations.
     *
     * Maps component type names (e.g. "Transform", "HealthComponent") to
     * type-erased add/has/remove/getRaw callbacks. Works with any World
     * class that supports the template-based AddComponent/HasComponent/etc API.
     *
     * ### Usage
     * ```cpp
     * auto& factory = ComponentFactory::Get();
     * factory.AddComponent("Transform", &world, entityId);
     * void* raw = factory.GetComponentRaw("Transform", &world, entityId);
     * ```
     *
     * @threadsafety Registration happens at static init time (single-threaded).
     *               Operations are read-only after startup.
     */
    class ComponentFactory
    {
      public:
        static ComponentFactory& Get()
        {
            static ComponentFactory instance;
            return instance;
        }

        /** @brief Register component operations for a type name. */
        void Register(const std::string& name, ComponentOps ops) { m_ops[name] = ops; }

        /** @brief Add a default-constructed component to an entity. Returns true if the type is known. */
        bool AddComponent(const std::string& typeName, void* world, uint32_t entity) const
        {
            auto it = m_ops.find(typeName);
            if (it == m_ops.end() || !it->second.add)
                return false;
            it->second.add(world, entity);
            return true;
        }

        /** @brief Check if an entity has a component by type name. */
        bool HasComponent(const std::string& typeName, void* world, uint32_t entity) const
        {
            auto it = m_ops.find(typeName);
            if (it == m_ops.end() || !it->second.has)
                return false;
            return it->second.has(world, entity);
        }

        /** @brief Remove a component by type name. Returns true if the type is known. */
        bool RemoveComponent(const std::string& typeName, void* world, uint32_t entity) const
        {
            auto it = m_ops.find(typeName);
            if (it == m_ops.end() || !it->second.remove)
                return false;
            it->second.remove(world, entity);
            return true;
        }

        /** @brief Get a raw pointer to a component by type name. Returns nullptr if not found. */
        void* GetComponentRaw(const std::string& typeName, void* world, uint32_t entity) const
        {
            auto it = m_ops.find(typeName);
            if (it == m_ops.end() || !it->second.getRaw)
                return nullptr;
            return it->second.getRaw(world, entity);
        }

        /** @brief Check if a type name is registered. */
        bool IsRegistered(const std::string& typeName) const { return m_ops.count(typeName) > 0; }

        /** @brief Get all registered component type names. */
        std::vector<std::string> GetRegisteredNames() const
        {
            std::vector<std::string> names;
            names.reserve(m_ops.size());
            for (const auto& [name, _] : m_ops)
                names.push_back(name);
            return names;
        }

        /** @brief Get the number of registered component types. */
        size_t GetRegisteredCount() const { return m_ops.size(); }

        /**
         * @brief Set a reflected field on a component by field name, using string value.
         *
         * Looks up the TypeInfo for the given component type, finds the field,
         * then calls SetFieldFromString.
         *
         * @return true if the field was found and set successfully.
         */
        bool SetField(const std::string& typeName, void* component, const std::string& fieldName,
                      const std::string& value) const
        {
            const auto* typeInfo = TypeRegistry::Get().FindTypeByName(typeName);
            if (!typeInfo)
                return false;
            const auto* field = typeInfo->FindField(fieldName);
            if (!field)
                return false;
            return SetFieldFromString(component, *field, value);
        }

      private:
        ComponentFactory() = default;
        std::unordered_map<std::string, ComponentOps> m_ops;
    };

} // namespace Spark

// ============================================================================
// Registration macros
// ============================================================================

/**
 * @brief Begin type reflection registration.
 *
 * Place in one .cpp file per type. Registers the type at static init time.
 */
#define SPARK_REFLECT_TYPE(Type)                                                                                       \
    namespace                                                                                                          \
    {                                                                                                                  \
        struct SparkReflect_##Type                                                                                     \
        {                                                                                                              \
            SparkReflect_##Type()                                                                                      \
            {                                                                                                          \
                auto& info =                                                                                           \
                    Spark::TypeRegistry::Get().RegisterType(GetTypeId<Type>(), #Type, sizeof(Type), alignof(Type));

/**
 * @brief Begin type reflection with a base class.
 */
#define SPARK_REFLECT_TYPE_BASE(Type, BaseType)                                                                        \
    namespace                                                                                                          \
    {                                                                                                                  \
        struct SparkReflect_##Type                                                                                     \
        {                                                                                                              \
            SparkReflect_##Type()                                                                                      \
            {                                                                                                          \
                auto& info = Spark::TypeRegistry::Get().RegisterType(GetTypeId<Type>(), #Type, sizeof(Type),           \
                                                                     alignof(Type), GetTypeId<BaseType>());

/**
 * @brief Register a field within a SPARK_REFLECT_TYPE block.
 */
#define SPARK_REFLECT_FIELD(Type, member, displayName)                                                                 \
    {                                                                                                                  \
        Spark::FieldInfo field;                                                                                        \
        field.name = displayName;                                                                                      \
        field.fieldName = #member;                                                                                     \
        field.type = Spark::DeduceFieldType<decltype(Type::member)>();                                                 \
        field.offset = offsetof(Type, member);                                                                         \
        field.size = sizeof(Type::member);                                                                             \
        field.ownerType = GetTypeId<Type>();                                                                           \
        info.fields.push_back(field);                                                                                  \
    }

/**
 * @brief Register a field with editor range constraints.
 */
#define SPARK_REFLECT_FIELD_RANGE(Type, member, displayName, minVal, maxVal)                                           \
    {                                                                                                                  \
        Spark::FieldInfo field;                                                                                        \
        field.name = displayName;                                                                                      \
        field.fieldName = #member;                                                                                     \
        field.type = Spark::DeduceFieldType<decltype(Type::member)>();                                                 \
        field.offset = offsetof(Type, member);                                                                         \
        field.size = sizeof(Type::member);                                                                             \
        field.ownerType = GetTypeId<Type>();                                                                           \
        field.rangeMin = static_cast<float>(minVal);                                                                   \
        field.rangeMax = static_cast<float>(maxVal);                                                                   \
        field.hasRange = true;                                                                                         \
        info.fields.push_back(field);                                                                                  \
    }

/**
 * @brief End type reflection registration.
 * @param Type Must match the type name from SPARK_REFLECT_TYPE.
 */
#define SPARK_REFLECT_END(Type)                                                                                        \
    }                                                                                                                  \
    }                                                                                                                  \
    ;                                                                                                                  \
    static SparkReflect_##Type s_sparkReflect_##Type;                                                                  \
    }
