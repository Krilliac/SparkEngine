/**
 * @file FuzzReflectionBinaryProduction.cpp
 * @brief libc++-compiled production adapter for the reflection binary libFuzzer harness.
 *
 * Spark::DeserializeFromBinary (Core/ReflectionSerializer.h) decodes untrusted
 * [index:u16][tag:u8][size][bytes] records into any reflected struct. The adapter describes
 * one record with every field kind the codec knows: Bool, Int, Float, Double, Vector2/3/4,
 * Enum, String, a Custom field that owns heap memory, an Unknown field, a field excluded from
 * serialization and a second String. It decodes the fuzz bytes into a sentinel record. A
 * violated contract aborts so libFuzzer records a crash:
 *  - the decoder consumes either nothing (an error) or every input byte,
 *  - the Custom, Unknown and non-serialized fields are never written, whatever the input,
 *  - the Bool field's object representation is exactly 0 or 1,
 *  - a fully consumed input re-encodes (SerializeToBinary) to bytes that decode into the same
 *    record and re-encode identically, so encode and decode agree on every accepted record.
 */

#include "FuzzReflectionBinaryProduction.h"

#include "Core/ReflectionSerializer.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzReflectionBinary: DeserializeFromBinary violated: %s\n", what);
        std::abort();
    }

    enum class Mode : std::uint32_t
    {
        Idle = 0,
        Active = 7
    };

    struct Record
    {
        bool flag = true;
        int count = -12;
        float scale = 1.5f;
        double precise = 2.25;
        std::array<float, 2> v2{1.0f, 2.0f};
        std::array<float, 3> v3{3.0f, 4.0f, 5.0f};
        std::array<float, 4> v4{6.0f, 7.0f, 8.0f, 9.0f};
        Mode mode = Mode::Active;
        std::string name = "sentinel-name";
        std::vector<std::uint32_t> custom{0xC0FFEEu, 0xBADF00Du}; // Custom: owns heap memory
        std::array<std::uint8_t, 4> unknown{0xA5, 0x5A, 0xA5, 0x5A};
        int hidden = 4242; // serialized = false
        std::string note = "sentinel-note";
    };

    Spark::FieldInfo Field(const char* name, Spark::FieldType type, std::size_t offset, std::size_t size,
                           bool serialized = true)
    {
        Spark::FieldInfo field;
        field.name = name;
        field.fieldName = name;
        field.type = type;
        field.offset = offset;
        field.size = size;
        field.ownerType = nullptr;
        field.serialized = serialized;
        return field;
    }

    const Spark::TypeInfo& RecordType()
    {
        static const Spark::TypeInfo type = []
        {
            using Spark::FieldType;
            Spark::TypeInfo info;
            info.name = "SparkFuzzReflectionRecord";
            info.size = sizeof(Record);
            info.alignment = alignof(Record);
            info.fields = {
                Field("flag", FieldType::Bool, offsetof(Record, flag), sizeof(bool)),
                Field("count", FieldType::Int, offsetof(Record, count), sizeof(int)),
                Field("scale", FieldType::Float, offsetof(Record, scale), sizeof(float)),
                Field("precise", FieldType::Double, offsetof(Record, precise), sizeof(double)),
                Field("v2", FieldType::Vector2, offsetof(Record, v2), sizeof(Record::v2)),
                Field("v3", FieldType::Vector3, offsetof(Record, v3), sizeof(Record::v3)),
                Field("v4", FieldType::Vector4, offsetof(Record, v4), sizeof(Record::v4)),
                Field("mode", FieldType::Enum, offsetof(Record, mode), sizeof(Mode)),
                Field("name", FieldType::String, offsetof(Record, name), sizeof(std::string)),
                Field("custom", FieldType::Custom, offsetof(Record, custom), sizeof(Record::custom)),
                Field("unknown", FieldType::Unknown, offsetof(Record, unknown), sizeof(Record::unknown)),
                Field("hidden", FieldType::Int, offsetof(Record, hidden), sizeof(int), false),
                Field("note", FieldType::String, offsetof(Record, note), sizeof(std::string)),
            };
            return info;
        }();
        return type;
    }

    std::vector<std::uint8_t> Encode(const Record& record)
    {
        std::vector<std::uint8_t> out;
        Spark::SerializeToBinary(&record, RecordType(), out);
        return out;
    }

    void CheckUntouchable(const Record& record)
    {
        const Record sentinel;
        if (record.custom != sentinel.custom)
            InvariantFailure("the Custom field was written");
        if (record.unknown != sentinel.unknown)
            InvariantFailure("the Unknown field was written");
        if (record.hidden != sentinel.hidden)
            InvariantFailure("a non-serialized field was written");
        std::uint8_t flagByte = 0;
        std::memcpy(&flagByte, &record.flag, 1);
        if (flagByte > 1)
            InvariantFailure("the Bool field holds a byte other than 0 or 1");
    }
} // namespace

extern "C" int SparkFuzzDeserializeReflectionBinary(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
        return 0;
    // DeserializeFromBinary treats a null buffer as an error; hand it a real one.
    const std::vector<std::uint8_t> input(data, data + size);
    const std::uint8_t* buffer = input.empty() ? reinterpret_cast<const std::uint8_t*>("") : input.data();

    Record record;
    const std::size_t consumed = Spark::DeserializeFromBinary(&record, RecordType(), buffer, input.size());
    if (consumed != 0 && consumed != input.size())
        InvariantFailure("the decoder consumed part of the input");
    CheckUntouchable(record);
    if (consumed != input.size())
        return 0;

    const std::vector<std::uint8_t> encoded = Encode(record);
    Record reloaded;
    if (Spark::DeserializeFromBinary(&reloaded, RecordType(), encoded.data(), encoded.size()) != encoded.size())
        InvariantFailure("the decoder rejects what the encoder wrote");
    CheckUntouchable(reloaded);
    if (Encode(reloaded) != encoded)
        InvariantFailure("encode -> decode -> encode changed the record");
    return 0;
}
