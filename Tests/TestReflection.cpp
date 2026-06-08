// TestReflection.cpp - Tests for the TypeRegistry / Reflection system
// Standalone implementations for CI testing

#include "TestFramework.h"
#include "TestCommonMath.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
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
        // Non-const so MSVC /OPT:ICF (Release) cannot fold these per-type markers
        // into one address. See EngineContext::GetTypeId for the full rationale.
        static char id;
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
        Vector2,
        Vector3,
        Vector4,
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
    using TestMath::Vec3;

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

// ============================================================================
// SetFieldFromString / GetFieldAsString tests (standalone implementation)
// ============================================================================

namespace TestReflect
{

    /// Standalone implementation matching Spark::SetFieldFromString
    bool SetFieldFromString(void* component, const FieldInfo& field, const std::string& value)
    {
        auto* dst = static_cast<char*>(component) + field.offset;
        switch (field.type)
        {
        case FieldType::Bool:
        {
            bool v = (value == "true" || value == "1" || value == "yes");
            std::memcpy(dst, &v, sizeof(bool));
            return true;
        }
        case FieldType::Int:
        {
            try
            {
                int v = std::stoi(value);
                std::memcpy(dst, &v, sizeof(int));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        case FieldType::Float:
        {
            try
            {
                float v = std::stof(value);
                std::memcpy(dst, &v, sizeof(float));
                return true;
            }
            catch (...)
            {
                return false;
            }
        }
        case FieldType::String:
        {
            auto* str = reinterpret_cast<std::string*>(dst);
            *str = value;
            return true;
        }
        default:
            return false;
        }
    }

    std::string GetFieldAsString(const void* component, const FieldInfo& field)
    {
        const auto* src = static_cast<const char*>(component) + field.offset;
        switch (field.type)
        {
        case FieldType::Bool:
        {
            bool v;
            std::memcpy(&v, src, sizeof(bool));
            return v ? "true" : "false";
        }
        case FieldType::Int:
        {
            int v;
            std::memcpy(&v, src, sizeof(int));
            return std::to_string(v);
        }
        case FieldType::Float:
        {
            float v;
            std::memcpy(&v, src, sizeof(float));
            return std::to_string(v);
        }
        case FieldType::String:
        {
            const auto* str = reinterpret_cast<const std::string*>(src);
            return *str;
        }
        default:
            return "";
        }
    }

} // namespace TestReflect

TEST(Reflection_SetFieldFromString_Float)
{
    Light light;
    light.intensity = 1.0f;

    FieldInfo field;
    field.fieldName = "intensity";
    field.type = FieldType::Float;
    field.offset = offsetof(Light, intensity);
    field.size = sizeof(float);

    bool ok = TestReflect::SetFieldFromString(&light, field, "42.5");
    EXPECT_TRUE(ok);
    EXPECT_NEAR(light.intensity, 42.5f, 0.001f);
}

TEST(Reflection_SetFieldFromString_Bool)
{
    MeshRenderer mr;
    mr.castShadows = false;

    FieldInfo field;
    field.fieldName = "castShadows";
    field.type = FieldType::Bool;
    field.offset = offsetof(MeshRenderer, castShadows);
    field.size = sizeof(bool);

    bool ok = TestReflect::SetFieldFromString(&mr, field, "true");
    EXPECT_TRUE(ok);
    EXPECT_TRUE(mr.castShadows);
}

TEST(Reflection_SetFieldFromString_Int)
{
    struct TestStruct
    {
        int value = 0;
    } ts;

    FieldInfo field;
    field.fieldName = "value";
    field.type = FieldType::Int;
    field.offset = 0;
    field.size = sizeof(int);

    bool ok = TestReflect::SetFieldFromString(&ts, field, "99");
    EXPECT_TRUE(ok);
    EXPECT_EQ(ts.value, 99);
}

TEST(Reflection_SetFieldFromString_String)
{
    MeshRenderer mr;
    mr.meshPath = "old.mesh";

    FieldInfo field;
    field.fieldName = "meshPath";
    field.type = FieldType::String;
    field.offset = offsetof(MeshRenderer, meshPath);
    field.size = sizeof(std::string);

    bool ok = TestReflect::SetFieldFromString(&mr, field, "models/crate.mesh");
    EXPECT_TRUE(ok);
    EXPECT_EQ(mr.meshPath, std::string("models/crate.mesh"));
}

TEST(Reflection_GetFieldAsString_Float)
{
    Light light;
    light.intensity = 7.5f;

    FieldInfo field;
    field.fieldName = "intensity";
    field.type = FieldType::Float;
    field.offset = offsetof(Light, intensity);
    field.size = sizeof(float);

    std::string result = TestReflect::GetFieldAsString(&light, field);
    EXPECT_TRUE(!result.empty());
    // Parse back and verify
    float parsed = std::stof(result);
    EXPECT_NEAR(parsed, 7.5f, 0.001f);
}

TEST(Reflection_GetFieldAsString_Bool)
{
    MeshRenderer mr;
    mr.castShadows = true;

    FieldInfo field;
    field.fieldName = "castShadows";
    field.type = FieldType::Bool;
    field.offset = offsetof(MeshRenderer, castShadows);
    field.size = sizeof(bool);

    std::string result = TestReflect::GetFieldAsString(&mr, field);
    EXPECT_EQ(result, std::string("true"));
}

TEST(Reflection_SetFieldFromString_InvalidFloat)
{
    Light light;
    light.intensity = 1.0f;

    FieldInfo field;
    field.fieldName = "intensity";
    field.type = FieldType::Float;
    field.offset = offsetof(Light, intensity);
    field.size = sizeof(float);

    bool ok = TestReflect::SetFieldFromString(&light, field, "not_a_number");
    EXPECT_TRUE(!ok);
    // Value should be unchanged
    EXPECT_NEAR(light.intensity, 1.0f, 0.001f);
}

TEST(Reflection_SetFieldFromString_UnsupportedType)
{
    Transform t;

    FieldInfo field;
    field.fieldName = "position";
    field.type = FieldType::Custom;
    field.offset = offsetof(Transform, position);
    field.size = sizeof(Vec3);

    bool ok = TestReflect::SetFieldFromString(&t, field, "1,2,3");
    // Custom type is unsupported in this standalone impl
    EXPECT_TRUE(!ok);
}

TEST(Reflection_SerializeRoundTrip_MultipleFields)
{
    // Test that multiple fields on the same struct can be set and read back
    auto& reg = TypeRegistry::Get();
    reg.Clear();

    auto& info =
        reg.RegisterType(GetTypeId<MeshRenderer>(), "MeshRenderer", sizeof(MeshRenderer), alignof(MeshRenderer));

    FieldInfo pathField;
    pathField.fieldName = "meshPath";
    pathField.type = FieldType::String;
    pathField.offset = offsetof(MeshRenderer, meshPath);
    pathField.size = sizeof(std::string);
    info.fields.push_back(pathField);

    FieldInfo shadowField;
    shadowField.fieldName = "castShadows";
    shadowField.type = FieldType::Bool;
    shadowField.offset = offsetof(MeshRenderer, castShadows);
    shadowField.size = sizeof(bool);
    info.fields.push_back(shadowField);

    // Set fields
    MeshRenderer mr;
    TestReflect::SetFieldFromString(&mr, *info.FindField("meshPath"), "models/hero.mesh");
    TestReflect::SetFieldFromString(&mr, *info.FindField("castShadows"), "false");

    EXPECT_EQ(mr.meshPath, std::string("models/hero.mesh"));
    EXPECT_TRUE(!mr.castShadows);

    // Read back
    std::string pathStr = TestReflect::GetFieldAsString(&mr, *info.FindField("meshPath"));
    EXPECT_EQ(pathStr, std::string("models/hero.mesh"));

    std::string shadowStr = TestReflect::GetFieldAsString(&mr, *info.FindField("castShadows"));
    EXPECT_EQ(shadowStr, std::string("false"));
}

TEST(Reflection_FindTypeByName_MultipleTypes)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();

    reg.RegisterType(GetTypeId<Transform>(), "Transform", sizeof(Transform), alignof(Transform));
    reg.RegisterType(GetTypeId<Light>(), "Light", sizeof(Light), alignof(Light));
    reg.RegisterType(GetTypeId<MeshRenderer>(), "MeshRenderer", sizeof(MeshRenderer), alignof(MeshRenderer));

    EXPECT_TRUE(reg.GetTypeCount() == 3u);
    EXPECT_TRUE(reg.FindTypeByName("Transform") != nullptr);
    EXPECT_TRUE(reg.FindTypeByName("Light") != nullptr);
    EXPECT_TRUE(reg.FindTypeByName("MeshRenderer") != nullptr);
    EXPECT_TRUE(reg.FindTypeByName("NonExistent") == nullptr);
}

TEST(Reflection_RoundTrip_AllTypes)
{
    auto& reg = TypeRegistry::Get();
    reg.Clear();
    auto& info = reg.RegisterType(GetTypeId<Light>(), "Light", sizeof(Light), alignof(Light));

    // Register intensity field
    FieldInfo intensityField;
    intensityField.fieldName = "intensity";
    intensityField.type = FieldType::Float;
    intensityField.offset = offsetof(Light, intensity);
    intensityField.size = sizeof(float);
    info.fields.push_back(intensityField);

    // Register range field
    FieldInfo rangeField;
    rangeField.fieldName = "range";
    rangeField.type = FieldType::Float;
    rangeField.offset = offsetof(Light, range);
    rangeField.size = sizeof(float);
    info.fields.push_back(rangeField);

    // Set both fields via reflection
    Light light;
    const auto* intensityF = info.FindField("intensity");
    const auto* rangeF = info.FindField("range");
    EXPECT_TRUE(intensityF != nullptr);
    EXPECT_TRUE(rangeF != nullptr);

    TestReflect::SetFieldFromString(&light, *intensityF, "3.14");
    TestReflect::SetFieldFromString(&light, *rangeF, "25.0");

    EXPECT_NEAR(light.intensity, 3.14f, 0.01f);
    EXPECT_NEAR(light.range, 25.0f, 0.01f);

    // Read back
    std::string intensityStr = TestReflect::GetFieldAsString(&light, *intensityF);
    float parsed = std::stof(intensityStr);
    EXPECT_NEAR(parsed, 3.14f, 0.01f);
}
