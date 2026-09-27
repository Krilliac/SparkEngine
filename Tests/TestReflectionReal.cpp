/**
 * @file TestReflectionReal.cpp
 * @brief Real-class tests for Spark::TypeRegistry, FieldInfo attributes,
 *        Vector2 support, enum fields, categories, and ReflectionSerializer
 */

#include "TestFramework.h"
#include "Core/Reflection.h"
#include "Core/ReflectionSerializer.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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
        int customBlob[2] = {5, 6}; // Registered as FieldType::Custom by the BinaryHardening fixture
    };

    enum class ReflAttrMetadataFixture
    {
        Properties,
        SerializedFilter,
        Binary,
        HealthOnly,
        BinaryHardening
    };

    Spark::TypeInfo& RebuildReflAttrTestMetadata(ReflAttrMetadataFixture fixture)
    {
        auto& registry = Spark::TypeRegistry::Get();
        auto& info = registry.RegisterType(::GetTypeId<ReflAttr_TestStruct>(), "ReflAttr_TestStruct",
                                           sizeof(ReflAttr_TestStruct), alignof(ReflAttr_TestStruct));
        info.fields.clear();

        const auto addField =
            [&info](const char* fieldName, Spark::FieldType type, size_t offset, size_t size, bool serialized = true)
        {
            Spark::FieldInfo field;
            field.fieldName = fieldName;
            field.type = type;
            field.offset = offset;
            field.size = size;
            field.serialized = serialized;
            info.fields.push_back(std::move(field));
        };

        switch (fixture)
        {
        case ReflAttrMetadataFixture::Properties:
            addField("health", Spark::FieldType::Float, offsetof(ReflAttr_TestStruct, health), sizeof(float));
            addField("active", Spark::FieldType::Bool, offsetof(ReflAttr_TestStruct, active), sizeof(bool));
            addField("name", Spark::FieldType::String, offsetof(ReflAttr_TestStruct, name), sizeof(std::string));
            break;
        case ReflAttrMetadataFixture::SerializedFilter:
            addField("health", Spark::FieldType::Float, offsetof(ReflAttr_TestStruct, health), sizeof(float));
            addField("maxHealth", Spark::FieldType::Float, offsetof(ReflAttr_TestStruct, maxHealth), sizeof(float),
                     false);
            break;
        case ReflAttrMetadataFixture::Binary:
            addField("health", Spark::FieldType::Float, offsetof(ReflAttr_TestStruct, health), sizeof(float));
            addField("mode", Spark::FieldType::Int, offsetof(ReflAttr_TestStruct, mode), sizeof(int));
            break;
        case ReflAttrMetadataFixture::HealthOnly:
            addField("health", Spark::FieldType::Float, offsetof(ReflAttr_TestStruct, health), sizeof(float));
            break;
        case ReflAttrMetadataFixture::BinaryHardening:
            // Indices: 0 health, 1 mode, 2 active, 3 name, 4 customBlob (Custom), 5 maxHealth (not serialized)
            addField("health", Spark::FieldType::Float, offsetof(ReflAttr_TestStruct, health), sizeof(float));
            addField("mode", Spark::FieldType::Int, offsetof(ReflAttr_TestStruct, mode), sizeof(int));
            addField("active", Spark::FieldType::Bool, offsetof(ReflAttr_TestStruct, active), sizeof(bool));
            addField("name", Spark::FieldType::String, offsetof(ReflAttr_TestStruct, name), sizeof(std::string));
            addField("customBlob", Spark::FieldType::Custom, offsetof(ReflAttr_TestStruct, customBlob),
                     sizeof(ReflAttr_TestStruct::customBlob));
            addField("maxHealth", Spark::FieldType::Float, offsetof(ReflAttr_TestStruct, maxHealth), sizeof(float),
                     false);
            break;
        }

        return info;
    }

    // Appends one fixed-size wire record: [index:u16][tag:u8][size:u16][payload].
    void AppendFixedRecord(std::vector<uint8_t>& out, uint16_t index, Spark::FieldType tag,
                           const std::vector<uint8_t>& payload)
    {
        const auto size = static_cast<uint16_t>(payload.size());
        out.push_back(static_cast<uint8_t>(index & 0xFF));
        out.push_back(static_cast<uint8_t>(index >> 8));
        out.push_back(static_cast<uint8_t>(tag));
        out.push_back(static_cast<uint8_t>(size & 0xFF));
        out.push_back(static_cast<uint8_t>(size >> 8));
        out.insert(out.end(), payload.begin(), payload.end());
    }

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
    auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::Properties);

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
    RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::Properties);
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
    RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::Properties);
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
    auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::SerializedFilter);

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
    auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::Binary);

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
    RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::HealthOnly);

    ReflAttr_TestStruct s;
    s.health = 33.3f;

    auto props = Spark::SerializeByName(&s, "ReflAttr_TestStruct");
    EXPECT_EQ(props.size(), 1u);

    // Unknown type returns empty
    auto empty = Spark::SerializeByName(&s, "NonExistent");
    EXPECT_TRUE(empty.empty());
}

// ============================================================================
// SEC4: DeserializeFromBinary treats its buffer as untrusted input
// ============================================================================

TEST(SEC4Reflection_BinaryTagMismatchOnStringFieldIgnored)
{
    const auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::BinaryHardening);

    // An Int-tagged record sized like a std::string that targets the String field
    // must not be raw-copied over the string object. All-zero bytes keep the
    // pre-fix failure observable on release standard libraries without planting a
    // freeable pointer: the name just reads empty.
    std::vector<uint8_t> buffer;
    AppendFixedRecord(buffer, 3, Spark::FieldType::Int, std::vector<uint8_t>(sizeof(std::string), 0));

    ReflAttr_TestStruct restored;
    const size_t consumed = Spark::DeserializeFromBinary(&restored, info, buffer.data(), buffer.size());
    EXPECT_EQ(consumed, buffer.size());
    EXPECT_EQ(restored.name, std::string("default"));
}

TEST(SEC4Reflection_BinaryTagMismatchOnPlainFieldIgnored)
{
    const auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::BinaryHardening);

    // A Float-tagged record for the Int field has the right size but the wrong type.
    const float wireValue = 1.5f;
    std::vector<uint8_t> payload(sizeof(float));
    std::memcpy(payload.data(), &wireValue, sizeof(float));
    std::vector<uint8_t> buffer;
    AppendFixedRecord(buffer, 1, Spark::FieldType::Float, payload);

    ReflAttr_TestStruct restored;
    restored.mode = 3;
    const size_t consumed = Spark::DeserializeFromBinary(&restored, info, buffer.data(), buffer.size());
    EXPECT_EQ(consumed, buffer.size());
    EXPECT_EQ(restored.mode, 3);
}

TEST(SEC4Reflection_BinaryCustomAndUnserializedFieldsNeverWritten)
{
    const auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::BinaryHardening);

    std::vector<uint8_t> buffer;
    AppendFixedRecord(buffer, 4, Spark::FieldType::Custom, std::vector<uint8_t>(sizeof(int) * 2, 0xEE));
    const float wireHealth = 999.0f;
    std::vector<uint8_t> healthPayload(sizeof(float));
    std::memcpy(healthPayload.data(), &wireHealth, sizeof(float));
    AppendFixedRecord(buffer, 5, Spark::FieldType::Float, healthPayload);

    ReflAttr_TestStruct restored;
    const size_t consumed = Spark::DeserializeFromBinary(&restored, info, buffer.data(), buffer.size());
    EXPECT_EQ(consumed, buffer.size());
    EXPECT_EQ(restored.customBlob[0], 5);
    EXPECT_EQ(restored.customBlob[1], 6);
    EXPECT_NEAR(restored.maxHealth, 100.0f, 0.001f);

    // The encoder must not emit the opaque Custom field or the unserialized field either.
    ReflAttr_TestStruct original;
    std::vector<uint8_t> encoded;
    Spark::SerializeToBinary(&original, info, encoded);
    const size_t expectedSize =
        (5 + sizeof(float)) + (5 + sizeof(int)) + (5 + sizeof(bool)) + (3 + 4 + original.name.size());
    EXPECT_EQ(encoded.size(), expectedSize);

    ReflAttr_TestStruct roundTrip;
    roundTrip.health = 0.0f;
    roundTrip.mode = -1;
    roundTrip.active = false;
    roundTrip.name.clear();
    EXPECT_EQ(Spark::DeserializeFromBinary(&roundTrip, info, encoded.data(), encoded.size()), encoded.size());
    EXPECT_NEAR(roundTrip.health, original.health, 0.001f);
    EXPECT_EQ(roundTrip.mode, original.mode);
    EXPECT_TRUE(roundTrip.active);
    EXPECT_EQ(roundTrip.name, original.name);
}

TEST(SEC4Reflection_BinaryBoolNormalized)
{
    const auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::BinaryHardening);

    std::vector<uint8_t> buffer;
    AppendFixedRecord(buffer, 2, Spark::FieldType::Bool, std::vector<uint8_t>(sizeof(bool), 0x7F));

    ReflAttr_TestStruct restored;
    restored.active = false;
    const size_t consumed = Spark::DeserializeFromBinary(&restored, info, buffer.data(), buffer.size());
    EXPECT_EQ(consumed, buffer.size());

    // Inspect the object representation: a raw copy would leave 0x7F in the bool.
    unsigned char stored = 0;
    std::memcpy(&stored, &restored.active, sizeof(stored));
    EXPECT_EQ(static_cast<int>(stored), 1);
}

TEST(SEC4Reflection_BinaryTruncatedRecordReturnsZero)
{
    const auto& info = RebuildReflAttrTestMetadata(ReflAttrMetadataFixture::BinaryHardening);

    ReflAttr_TestStruct original;
    original.name = "truncated";
    std::vector<uint8_t> encoded;
    Spark::SerializeToBinary(&original, info, encoded);
    EXPECT_TRUE(encoded.size() > 1);

    // Dropping the last byte of the trailing string record is an error, not a partial read.
    ReflAttr_TestStruct restored;
    EXPECT_EQ(Spark::DeserializeFromBinary(&restored, info, encoded.data(), encoded.size() - 1), size_t{0});

    // A string length that runs past the buffer is also an error and leaves the field alone.
    const std::vector<uint8_t> hugeString = {3,    0,  static_cast<uint8_t>(Spark::FieldType::String), 0xFF, 0xFF, 0xFF,
                                             0xFF, 'x'};
    ReflAttr_TestStruct other;
    EXPECT_EQ(Spark::DeserializeFromBinary(&other, info, hugeString.data(), hugeString.size()), size_t{0});
    EXPECT_EQ(other.name, std::string("default"));
}
