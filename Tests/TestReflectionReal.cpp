/**
 * @file TestReflectionReal.cpp
 * @brief Real-class tests for Spark::TypeRegistry, FieldInfo attributes,
 *        Vector2 support, enum fields, categories, and ReflectionSerializer
 */

#include "TestFramework.h"
#include "Core/Reflection.h"
#include "Core/ReflectionSerializer.h"

#include <cstring>

namespace
{
    struct PhaseMM_TestType
    {
        int x = 0;
        float y = 0.0f;
    };

    struct PhaseMM_BaseType
    {
    };

    struct PhaseMM_DerivedType : public PhaseMM_BaseType
    {
    };

    // Test struct for extended attribute tests
    struct ReflAttr_TestStruct
    {
        float health = 100.0f;
        float maxHealth = 100.0f;
        bool active = true;
        int mode = 0;
        std::string name = "default";
    };

    // Test struct for Vector2 tests
    struct ReflVec2_TestStruct
    {
        float pos[2] = {0.0f, 0.0f}; // Simulates XMFLOAT2
        float vel[3] = {0.0f, 0.0f, 0.0f};
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    };

} // namespace

TEST(ReflectionReal_SingletonStable)
{
    auto& a = Spark::TypeRegistry::Get();
    auto& b = Spark::TypeRegistry::Get();
    EXPECT_TRUE(&a == &b);
}

TEST(ReflectionReal_RegisterAndFindType)
{
    auto& reg = Spark::TypeRegistry::Get();
    const auto id = ::GetTypeId<PhaseMM_TestType>();
    reg.RegisterType(id, "PhaseMM_TestType", sizeof(PhaseMM_TestType), alignof(PhaseMM_TestType));

    const auto* info = reg.FindType(id);
    EXPECT_TRUE(info != nullptr);
    if (info)
    {
        EXPECT_EQ(info->name, std::string("PhaseMM_TestType"));
        EXPECT_EQ(info->size, sizeof(PhaseMM_TestType));
        EXPECT_EQ(info->alignment, alignof(PhaseMM_TestType));
    }
}

TEST(ReflectionReal_FindByName)
{
    auto& reg = Spark::TypeRegistry::Get();
    reg.RegisterType(::GetTypeId<PhaseMM_TestType>(), "PhaseMM_TestType", sizeof(PhaseMM_TestType),
                     alignof(PhaseMM_TestType));

    const auto* info = reg.FindTypeByName("PhaseMM_TestType");
    EXPECT_TRUE(info != nullptr);
}

TEST(ReflectionReal_FindUnknownReturnsNull)
{
    auto& reg = Spark::TypeRegistry::Get();
    const auto* info = reg.FindTypeByName("DefinitelyNotRegistered_PhaseMM");
    EXPECT_TRUE(info == nullptr);
}

TEST(ReflectionReal_GetAllTypesNonEmpty)
{
    auto& reg = Spark::TypeRegistry::Get();
    reg.RegisterType(::GetTypeId<PhaseMM_TestType>(), "PhaseMM_TestType", sizeof(PhaseMM_TestType),
                     alignof(PhaseMM_TestType));
    const auto& all = reg.GetAllTypes();
    EXPECT_TRUE(all.size() >= static_cast<size_t>(1));
}

TEST(ReflectionReal_GetTypeCountMatchesAllTypes)
{
    auto& reg = Spark::TypeRegistry::Get();
    EXPECT_EQ(reg.GetTypeCount(), reg.GetAllTypes().size());
}

TEST(ReflectionReal_DerivedType)
{
    auto& reg = Spark::TypeRegistry::Get();
    reg.RegisterType(::GetTypeId<PhaseMM_BaseType>(), "PhaseMM_BaseType", sizeof(PhaseMM_BaseType),
                     alignof(PhaseMM_BaseType));
    reg.RegisterType(::GetTypeId<PhaseMM_DerivedType>(), "PhaseMM_DerivedType", sizeof(PhaseMM_DerivedType),
                     alignof(PhaseMM_DerivedType), ::GetTypeId<PhaseMM_BaseType>());

    auto derived = reg.GetDerivedTypes<PhaseMM_BaseType>();
    EXPECT_TRUE(derived.size() >= static_cast<size_t>(1));
}

// ============================================================================
// Extended FieldInfo attribute tests
// ============================================================================

TEST(ReflectionReal_FieldInfoExtendedAttributes)
{
    Spark::FieldInfo f;
    f.name = "Health";
    f.fieldName = "health";
    f.type = Spark::FieldType::Float;
    f.offset = offsetof(ReflAttr_TestStruct, health);
    f.size = sizeof(float);

    // New attributes should have sensible defaults
    EXPECT_TRUE(f.tooltip.empty());
    EXPECT_TRUE(f.category.empty());
    EXPECT_TRUE(!f.isAssetPath);
    EXPECT_TRUE(!f.replicated);
    EXPECT_TRUE(f.serialized); // default true
    EXPECT_TRUE(f.enumNames.empty());

    // Set new attributes
    f.tooltip = "Current hit points";
    f.category = "Combat";
    f.replicated = true;

    EXPECT_EQ(f.tooltip, std::string("Current hit points"));
    EXPECT_EQ(f.category, std::string("Combat"));
    EXPECT_TRUE(f.replicated);
}

TEST(ReflectionReal_FieldInfoEnumNames)
{
    Spark::FieldInfo f;
    f.name = "Mode";
    f.fieldName = "mode";
    f.type = Spark::FieldType::Int;
    f.offset = offsetof(ReflAttr_TestStruct, mode);
    f.size = sizeof(int);
    f.enumNames = {"Idle", "Patrol", "Combat", "Dead"};

    EXPECT_EQ(f.enumNames.size(), 4u);
    EXPECT_EQ(f.enumNames[0], std::string("Idle"));
    EXPECT_EQ(f.enumNames[3], std::string("Dead"));
}

// ============================================================================
// TypeInfo helper tests (GetCategories, count helpers, version)
// ============================================================================

TEST(ReflectionReal_TypeInfoVersion)
{
    Spark::TypeInfo info;
    EXPECT_EQ(info.version, 0u); // default

    info.version = 3;
    EXPECT_EQ(info.version, 3u);
}

TEST(ReflectionReal_TypeInfoGetCategories)
{
    Spark::TypeInfo info;
    info.name = "TestStruct";

    Spark::FieldInfo f1;
    f1.fieldName = "a";
    f1.category = "Combat";
    info.fields.push_back(f1);

    Spark::FieldInfo f2;
    f2.fieldName = "b";
    f2.category = "Movement";
    info.fields.push_back(f2);

    Spark::FieldInfo f3;
    f3.fieldName = "c";
    f3.category = "Combat"; // duplicate
    info.fields.push_back(f3);

    Spark::FieldInfo f4;
    f4.fieldName = "d";
    // no category (empty string)
    info.fields.push_back(f4);

    auto cats = info.GetCategories();
    EXPECT_EQ(cats.size(), 3u); // "Combat", "Movement", ""
}

TEST(ReflectionReal_TypeInfoGetSerializedFieldCount)
{
    Spark::TypeInfo info;

    Spark::FieldInfo f1;
    f1.serialized = true;
    info.fields.push_back(f1);

    Spark::FieldInfo f2;
    f2.serialized = false;
    info.fields.push_back(f2);

    Spark::FieldInfo f3;
    f3.serialized = true;
    info.fields.push_back(f3);

    EXPECT_EQ(info.GetSerializedFieldCount(), 2u);
}

TEST(ReflectionReal_TypeInfoGetReplicatedFieldCount)
{
    Spark::TypeInfo info;

    Spark::FieldInfo f1;
    f1.replicated = true;
    info.fields.push_back(f1);

    Spark::FieldInfo f2;
    f2.replicated = false;
    info.fields.push_back(f2);

    EXPECT_EQ(info.GetReplicatedFieldCount(), 1u);
}

// ============================================================================
// Vector2 field type tests (Set/GetFieldAsString)
// ============================================================================

TEST(ReflectionReal_Vector2_SetFieldFromString)
{
    ReflVec2_TestStruct s;

    Spark::FieldInfo f;
    f.fieldName = "pos";
    f.type = Spark::FieldType::Vector2;
    f.offset = offsetof(ReflVec2_TestStruct, pos);
    f.size = sizeof(float) * 2;

    bool ok = Spark::SetFieldFromString(&s, f, "3.5,7.2");
    EXPECT_TRUE(ok);
    EXPECT_NEAR(s.pos[0], 3.5f, 0.001f);
    EXPECT_NEAR(s.pos[1], 7.2f, 0.001f);
}

TEST(ReflectionReal_Vector2_GetFieldAsString)
{
    ReflVec2_TestStruct s;
    s.pos[0] = 1.0f;
    s.pos[1] = 2.0f;

    Spark::FieldInfo f;
    f.fieldName = "pos";
    f.type = Spark::FieldType::Vector2;
    f.offset = offsetof(ReflVec2_TestStruct, pos);
    f.size = sizeof(float) * 2;

    std::string result = Spark::GetFieldAsString(&s, f);
    EXPECT_TRUE(!result.empty());
    // Should contain two comma-separated floats
    EXPECT_TRUE(result.find(',') != std::string::npos);
}

TEST(ReflectionReal_Vector3_RoundTrip)
{
    ReflVec2_TestStruct s;

    Spark::FieldInfo f;
    f.fieldName = "vel";
    f.type = Spark::FieldType::Vector3;
    f.offset = offsetof(ReflVec2_TestStruct, vel);
    f.size = sizeof(float) * 3;

    Spark::SetFieldFromString(&s, f, "1.0,2.5,3.7");
    EXPECT_NEAR(s.vel[0], 1.0f, 0.001f);
    EXPECT_NEAR(s.vel[1], 2.5f, 0.001f);
    EXPECT_NEAR(s.vel[2], 3.7f, 0.001f);

    std::string str = Spark::GetFieldAsString(&s, f);
    EXPECT_TRUE(!str.empty());
}

// ============================================================================
// ReflectionSerializer tests
// ============================================================================

TEST(ReflectionSerializer_SerializeToProperties)
{
    // Register a test type
    auto& reg = Spark::TypeRegistry::Get();
    auto& info = reg.RegisterType(::GetTypeId<ReflAttr_TestStruct>(), "ReflAttr_TestStruct",
                                  sizeof(ReflAttr_TestStruct), alignof(ReflAttr_TestStruct));
    info.fields.clear();

    Spark::FieldInfo healthField;
    healthField.fieldName = "health";
    healthField.type = Spark::FieldType::Float;
    healthField.offset = offsetof(ReflAttr_TestStruct, health);
    healthField.size = sizeof(float);
    info.fields.push_back(healthField);

    Spark::FieldInfo activeField;
    activeField.fieldName = "active";
    activeField.type = Spark::FieldType::Bool;
    activeField.offset = offsetof(ReflAttr_TestStruct, active);
    activeField.size = sizeof(bool);
    info.fields.push_back(activeField);

    Spark::FieldInfo nameField;
    nameField.fieldName = "name";
    nameField.type = Spark::FieldType::String;
    nameField.offset = offsetof(ReflAttr_TestStruct, name);
    nameField.size = sizeof(std::string);
    info.fields.push_back(nameField);

    ReflAttr_TestStruct s;
    s.health = 75.0f;
    s.active = false;
    s.name = "hero";

    auto props = Spark::SerializeToProperties(&s, info);
    EXPECT_EQ(props.size(), 3u);
    EXPECT_TRUE(props.find("health") != props.end());
    EXPECT_TRUE(props.find("active") != props.end());
    EXPECT_TRUE(props.find("name") != props.end());
    EXPECT_EQ(props["name"], std::string("hero"));
    EXPECT_EQ(props["active"], std::string("false"));
}

TEST(ReflectionSerializer_DeserializeFromProperties)
{
    auto& reg = Spark::TypeRegistry::Get();
    const auto* info = reg.FindTypeByName("ReflAttr_TestStruct");
    EXPECT_TRUE(info != nullptr);
    if (!info)
        return;

    ReflAttr_TestStruct s;
    s.health = 0.0f;
    s.active = true;
    s.name = "none";

    std::unordered_map<std::string, std::string> props;
    props["health"] = "42.0";
    props["active"] = "false";
    props["name"] = "restored";

    Spark::DeserializeFromProperties(&s, *info, props);
    EXPECT_NEAR(s.health, 42.0f, 0.01f);
    EXPECT_TRUE(!s.active);
    EXPECT_EQ(s.name, std::string("restored"));
}

TEST(ReflectionSerializer_RoundTrip)
{
    auto& reg = Spark::TypeRegistry::Get();
    const auto* info = reg.FindTypeByName("ReflAttr_TestStruct");
    EXPECT_TRUE(info != nullptr);
    if (!info)
        return;

    ReflAttr_TestStruct original;
    original.health = 88.5f;
    original.active = true;
    original.name = "test_entity";

    // Serialize
    auto props = Spark::SerializeToProperties(&original, *info);

    // Deserialize into fresh struct
    ReflAttr_TestStruct restored;
    Spark::DeserializeFromProperties(&restored, *info, props);

    EXPECT_NEAR(restored.health, 88.5f, 0.01f);
    EXPECT_TRUE(restored.active);
    EXPECT_EQ(restored.name, std::string("test_entity"));
}

TEST(ReflectionSerializer_SerializedFalseSkipped)
{
    auto& reg = Spark::TypeRegistry::Get();
    auto& info = reg.RegisterType(::GetTypeId<ReflAttr_TestStruct>(), "ReflAttr_TestStruct",
                                  sizeof(ReflAttr_TestStruct), alignof(ReflAttr_TestStruct));
    info.fields.clear();

    Spark::FieldInfo f1;
    f1.fieldName = "health";
    f1.type = Spark::FieldType::Float;
    f1.offset = offsetof(ReflAttr_TestStruct, health);
    f1.size = sizeof(float);
    f1.serialized = true;
    info.fields.push_back(f1);

    Spark::FieldInfo f2;
    f2.fieldName = "maxHealth";
    f2.type = Spark::FieldType::Float;
    f2.offset = offsetof(ReflAttr_TestStruct, maxHealth);
    f2.size = sizeof(float);
    f2.serialized = false; // should be skipped
    info.fields.push_back(f2);

    ReflAttr_TestStruct s;
    s.health = 50.0f;
    s.maxHealth = 200.0f;

    auto props = Spark::SerializeToProperties(&s, info);
    EXPECT_EQ(props.size(), 1u);
    EXPECT_TRUE(props.find("health") != props.end());
    EXPECT_TRUE(props.find("maxHealth") == props.end());
}

TEST(ReflectionSerializer_BinaryRoundTrip)
{
    auto& reg = Spark::TypeRegistry::Get();
    auto& info = reg.RegisterType(::GetTypeId<ReflAttr_TestStruct>(), "ReflAttr_TestStruct",
                                  sizeof(ReflAttr_TestStruct), alignof(ReflAttr_TestStruct));
    info.fields.clear();

    Spark::FieldInfo f1;
    f1.fieldName = "health";
    f1.type = Spark::FieldType::Float;
    f1.offset = offsetof(ReflAttr_TestStruct, health);
    f1.size = sizeof(float);
    info.fields.push_back(f1);

    Spark::FieldInfo f2;
    f2.fieldName = "mode";
    f2.type = Spark::FieldType::Int;
    f2.offset = offsetof(ReflAttr_TestStruct, mode);
    f2.size = sizeof(int);
    info.fields.push_back(f2);

    ReflAttr_TestStruct original;
    original.health = 99.9f;
    original.mode = 7;

    // Serialize to binary
    std::vector<uint8_t> buffer;
    Spark::SerializeToBinary(&original, info, buffer);
    EXPECT_TRUE(!buffer.empty());

    // Deserialize from binary
    ReflAttr_TestStruct restored;
    size_t consumed = Spark::DeserializeFromBinary(&restored, info, buffer.data(), buffer.size());
    EXPECT_TRUE(consumed > 0);
    EXPECT_NEAR(restored.health, 99.9f, 0.01f);
    EXPECT_EQ(restored.mode, 7);
}

TEST(ReflectionSerializer_SerializeByName)
{
    auto& reg = Spark::TypeRegistry::Get();
    auto& info = reg.RegisterType(::GetTypeId<ReflAttr_TestStruct>(), "ReflAttr_TestStruct",
                                  sizeof(ReflAttr_TestStruct), alignof(ReflAttr_TestStruct));
    info.fields.clear();

    Spark::FieldInfo f;
    f.fieldName = "health";
    f.type = Spark::FieldType::Float;
    f.offset = offsetof(ReflAttr_TestStruct, health);
    f.size = sizeof(float);
    info.fields.push_back(f);

    ReflAttr_TestStruct s;
    s.health = 33.3f;

    auto props = Spark::SerializeByName(&s, "ReflAttr_TestStruct");
    EXPECT_EQ(props.size(), 1u);

    // Unknown type returns empty
    auto empty = Spark::SerializeByName(&s, "NonExistent");
    EXPECT_TRUE(empty.empty());
}
