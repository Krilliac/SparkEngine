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

// Reuse the EngineContext TypeId pattern
using TypeId = const void*;

template <typename T> TypeId GetTypeId()
{
    static const char id = 0;
    return &id;
}

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
