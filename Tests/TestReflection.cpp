// TestReflection.cpp - Tests for the TypeRegistry / Reflection system
// Standalone implementations for CI testing

#include "TestFramework.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

// ============================================================================
// Standalone reflection system (mirrors Core/Reflection.h implementation)
// ============================================================================

namespace TestReflect
{

    using TypeId = const void*;

    template <typename T> TypeId GetTypeId()
    {
        static const char id = 0;
        return &id;
    }

    enum class FieldType : uint8_t
    {
        Unknown,
        Bool,
        Int,
        Float,
        Double,
        String,
        Enum,
        Custom
    };

    struct FieldInfo
    {
        std::string name;
        std::string fieldName;
        FieldType type;
        size_t offset;
        size_t size;
        float rangeMin = 0.0f;
        float rangeMax = 0.0f;
        bool hasRange = false;
    };

    struct TypeInfo
    {
        std::string name;
        TypeId typeId = nullptr;
        size_t size = 0;
        size_t alignment = 0;
        TypeId baseType = nullptr;
        std::vector<FieldInfo> fields;

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

    class TypeRegistry
    {
      public:
        static TypeRegistry& Get()
        {
            static TypeRegistry instance;
            return instance;
        }

        TypeInfo& RegisterType(TypeId id, const std::string& name, size_t sz, size_t align, TypeId base = nullptr)
        {
            auto& info = m_types[id];
            info.name = name;
            info.typeId = id;
            info.size = sz;
            info.alignment = align;
            info.baseType = base;
            return info;
        }

        const TypeInfo* FindType(TypeId id) const
        {
            auto it = m_types.find(id);
            return (it != m_types.end()) ? &it->second : nullptr;
        }

        const TypeInfo* FindTypeByName(std::string_view name) const
        {
            for (const auto& [id, info] : m_types)
            {
                if (info.name == name)
                    return &info;
            }
            return nullptr;
        }

        size_t GetTypeCount() const { return m_types.size(); }

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

        void Clear() { m_types.clear(); }

      private:
        TypeRegistry() = default;
        std::unordered_map<TypeId, TypeInfo> m_types;
    };

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

    // Test types
    struct Vec3
    {
        float x = 0, y = 0, z = 0;
    };

    struct Transform
    {
        Vec3 position;
        Vec3 rotation;
        Vec3 scale;
        bool visible = true;
    };

    struct Component
    {
    };

    struct MeshRenderer : Component
    {
        std::string meshPath;
        std::string materialPath;
        bool castShadows = true;
    };

    struct Light : Component
    {
        float intensity = 1.0f;
        float range = 10.0f;
    };

} // namespace TestReflect

using namespace TestReflect;

TEST(Reflection_RegisterType)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();

    auto& info = reg.RegisterType(GetTypeId<Transform>(), "Transform", sizeof(Transform), alignof(Transform));
    EXPECT_EQ(info.name, std::string("Transform"));
    EXPECT_EQ(info.size, sizeof(Transform));
    EXPECT_EQ(info.typeId, GetTypeId<Transform>());
}

TEST(Reflection_FindType)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();
    reg.RegisterType(GetTypeId<Transform>(), "Transform", sizeof(Transform), alignof(Transform));

    const auto* found = reg.FindType(GetTypeId<Transform>());
    EXPECT_TRUE(found != nullptr);
    EXPECT_EQ(found->name, std::string("Transform"));

    const auto* notFound = reg.FindType(GetTypeId<Light>());
    EXPECT_TRUE(notFound == nullptr);
}

TEST(Reflection_FindTypeByName)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();
    reg.RegisterType(GetTypeId<Transform>(), "Transform", sizeof(Transform), alignof(Transform));

    const auto* found = reg.FindTypeByName("Transform");
    EXPECT_TRUE(found != nullptr);

    const auto* notFound = reg.FindTypeByName("NonExistent");
    EXPECT_TRUE(notFound == nullptr);
}

TEST(Reflection_RegisterFields)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();
    auto& info = reg.RegisterType(GetTypeId<Transform>(), "Transform", sizeof(Transform), alignof(Transform));

    FieldInfo posField;
    posField.name = "Position";
    posField.fieldName = "position";
    posField.type = FieldType::Custom;
    posField.offset = offsetof(Transform, position);
    posField.size = sizeof(Vec3);
    info.fields.push_back(posField);

    FieldInfo visField;
    visField.name = "Visible";
    visField.fieldName = "visible";
    visField.type = DeduceFieldType<bool>();
    visField.offset = offsetof(Transform, visible);
    visField.size = sizeof(bool);
    info.fields.push_back(visField);

    EXPECT_EQ(info.fields.size(), 2u);

    const auto* pos = info.FindField("position");
    EXPECT_TRUE(pos != nullptr);
    EXPECT_EQ(pos->name, std::string("Position"));
    EXPECT_TRUE(pos->type == FieldType::Custom);

    const auto* vis = info.FindField("visible");
    EXPECT_TRUE(vis != nullptr);
    EXPECT_TRUE(vis->type == FieldType::Bool);
}

TEST(Reflection_FieldTypeDeduction)
{
    EXPECT_TRUE(DeduceFieldType<bool>() == FieldType::Bool);
    EXPECT_TRUE(DeduceFieldType<int>() == FieldType::Int);
    EXPECT_TRUE(DeduceFieldType<float>() == FieldType::Float);
    EXPECT_TRUE(DeduceFieldType<double>() == FieldType::Double);
    EXPECT_TRUE(DeduceFieldType<std::string>() == FieldType::String);
    EXPECT_TRUE(DeduceFieldType<Vec3>() == FieldType::Custom);
}

TEST(Reflection_DerivedTypes)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();

    reg.RegisterType(GetTypeId<Component>(), "Component", sizeof(Component), alignof(Component));
    reg.RegisterType(GetTypeId<MeshRenderer>(), "MeshRenderer", sizeof(MeshRenderer), alignof(MeshRenderer),
                     GetTypeId<Component>());
    reg.RegisterType(GetTypeId<Light>(), "Light", sizeof(Light), alignof(Light), GetTypeId<Component>());
    reg.RegisterType(GetTypeId<Transform>(), "Transform", sizeof(Transform), alignof(Transform));

    auto derived = reg.GetDerivedTypes<Component>();
    EXPECT_EQ(derived.size(), 2u);

    // Verify both MeshRenderer and Light are found
    bool hasMeshRenderer = false;
    bool hasLight = false;
    for (const auto* t : derived)
    {
        if (t->name == "MeshRenderer")
            hasMeshRenderer = true;
        if (t->name == "Light")
            hasLight = true;
    }
    EXPECT_TRUE(hasMeshRenderer);
    EXPECT_TRUE(hasLight);
}

TEST(Reflection_FieldRange)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();
    auto& info = reg.RegisterType(GetTypeId<Light>(), "Light", sizeof(Light), alignof(Light));

    FieldInfo intensityField;
    intensityField.name = "Intensity";
    intensityField.fieldName = "intensity";
    intensityField.type = FieldType::Float;
    intensityField.offset = offsetof(Light, intensity);
    intensityField.size = sizeof(float);
    intensityField.rangeMin = 0.0f;
    intensityField.rangeMax = 100.0f;
    intensityField.hasRange = true;
    info.fields.push_back(intensityField);

    const auto* f = info.FindField("intensity");
    EXPECT_TRUE(f != nullptr);
    EXPECT_TRUE(f->hasRange);
    EXPECT_NEAR(f->rangeMin, 0.0f, 0.001f);
    EXPECT_NEAR(f->rangeMax, 100.0f, 0.001f);
}
