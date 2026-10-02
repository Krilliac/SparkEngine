/**
 * @file ReflectionSerializer.h
 * @brief Generic reflection-driven serialization utilities
 *
 * Provides type-agnostic serialize/deserialize for any type registered in
 * TypeRegistry. Uses GetFieldAsString/SetFieldFromString (from Reflection.h)
 * to convert fields to/from string key-value maps or raw binary.
 *
 * No production subsystem calls these helpers today; only Tests/TestReflectionReal.cpp
 * does. The binary decoder is nonetheless written for untrusted input (see
 * Tools/fuzz-policy/parser-inventory.json, id "reflection-binary-codec"): it only
 * writes a field when the wire type tag matches the reflected type, it never
 * raw-copies String, Custom or Unknown fields, and it normalizes Bool values.
 *
 * Thread affinity: none (pure functions over caller-owned memory).
 * Allocation: property maps, output buffers and decoded strings allocate.
 *
 * @see Core/Reflection.h, Core/ComponentReflection.cpp
 */

#pragma once

#include "Reflection.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    /**
     * @brief Serialize all reflected fields of a type to a string property map.
     *
     * Iterates the TypeInfo's fields and calls GetFieldAsString for each.
     * Fields with serialized=false are skipped.
     *
     * @param data      Pointer to the struct instance.
     * @param typeInfo  Reflected type metadata.
     * @return Property map: fieldName → string value.
     */
    inline std::unordered_map<std::string, std::string> SerializeToProperties(const void* data,
                                                                              const TypeInfo& typeInfo)
    {
        std::unordered_map<std::string, std::string> props;
        if (!data)
            return props;

        for (const auto& field : typeInfo.fields)
        {
            if (!field.serialized)
                continue;
            props[field.fieldName] = GetFieldAsString(data, field);
        }
        return props;
    }

    /**
     * @brief Deserialize a string property map into a reflected type instance.
     *
     * For each field in the TypeInfo, looks up the fieldName in the property map
     * and calls SetFieldFromString. Missing keys are silently skipped.
     *
     * @param data      Pointer to the struct instance.
     * @param typeInfo  Reflected type metadata.
     * @param props     Property map: fieldName → string value.
     */
    inline void DeserializeFromProperties(void* data, const TypeInfo& typeInfo,
                                          const std::unordered_map<std::string, std::string>& props)
    {
        if (!data)
            return;

        for (const auto& field : typeInfo.fields)
        {
            if (!field.serialized)
                continue;
            auto it = props.find(field.fieldName);
            if (it != props.end())
            {
                SetFieldFromString(data, field, it->second);
            }
        }
    }

    /**
     * @brief Whether a field type is plain data that the binary codec may copy byte for byte.
     *
     * String has its own length-prefixed encoding. Custom is opaque (it may own heap
     * memory or have invariants), and Unknown has no defined layout, so neither is
     * ever raw-copied in either direction.
     */
    [[nodiscard]] constexpr bool IsBinaryPlainFieldType(FieldType type) noexcept
    {
        switch (type)
        {
        case FieldType::Bool:
        case FieldType::Int:
        case FieldType::Float:
        case FieldType::Double:
        case FieldType::Vector2:
        case FieldType::Vector3:
        case FieldType::Vector4:
        case FieldType::Enum:
            return true;
        case FieldType::Unknown:
        case FieldType::String:
        case FieldType::Custom:
            return false;
        }
        return false;
    }

    /**
     * @brief Serialize all reflected fields to a binary buffer.
     *
     * Format per field: [fieldIndex:uint16][typeTag:uint8][rawBytes:size]
     * Fields with serialized=false are skipped, and so are fields the format cannot
     * represent: Custom and Unknown fields, plain fields wider than 65535 bytes and
     * strings longer than 4 GiB.
     *
     * @param data      Pointer to the struct instance.
     * @param typeInfo  Reflected type metadata.
     * @param out       Output buffer (appended to).
     */
    inline void SerializeToBinary(const void* data, const TypeInfo& typeInfo, std::vector<uint8_t>& out)
    {
        if (!data)
            return;

        const auto* src = static_cast<const char*>(data);
        uint16_t fieldIndex = 0;

        for (const auto& field : typeInfo.fields)
        {
            const bool isString = field.type == FieldType::String;
            if (!field.serialized || (!isString && !IsBinaryPlainFieldType(field.type)) ||
                (!isString && field.size > 0xFFFFu))
            {
                ++fieldIndex;
                continue;
            }
            if (isString && reinterpret_cast<const std::string*>(src + field.offset)->size() > 0xFFFFFFFFu)
            {
                ++fieldIndex;
                continue;
            }

            // Field header: index + type tag
            out.push_back(static_cast<uint8_t>(fieldIndex & 0xFF));
            out.push_back(static_cast<uint8_t>((fieldIndex >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>(field.type));

            if (field.type == FieldType::String)
            {
                // Strings are length-prefixed: [uint32 len][chars]
                const auto* str = reinterpret_cast<const std::string*>(src + field.offset);
                auto len = static_cast<uint32_t>(str->size());
                out.push_back(static_cast<uint8_t>(len & 0xFF));
                out.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
                out.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
                out.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
                out.insert(out.end(), str->begin(), str->end());
            }
            else
            {
                // Fixed-size fields: copy raw bytes
                auto sz = static_cast<uint16_t>(field.size);
                out.push_back(static_cast<uint8_t>(sz & 0xFF));
                out.push_back(static_cast<uint8_t>((sz >> 8) & 0xFF));
                out.insert(out.end(), src + field.offset, src + field.offset + field.size);
            }

            ++fieldIndex;
        }
    }

    /**
     * @brief Deserialize a binary buffer into a reflected type instance.
     *
     * Reads field headers and raw data, matching by field index. A record is
     * applied only when its index names a serialized field whose reflected type
     * equals the wire type tag; for plain types the wire size must also equal the
     * field size. Records for unknown indices, mismatched types, non-serialized
     * fields, or Custom/Unknown fields are skipped without touching the instance
     * (forward-compatible). Bool values are normalized to true/false.
     *
     * @param data      Pointer to the struct instance.
     * @param typeInfo  Reflected type metadata.
     * @param buf       Input buffer (untrusted).
     * @param bufSize   Size of the input buffer.
     * @return Number of bytes consumed, or 0 on error (null input or a truncated
     *         record). Records before a truncated one may already have been applied.
     */
    inline size_t DeserializeFromBinary(void* data, const TypeInfo& typeInfo, const uint8_t* buf, size_t bufSize)
    {
        if (!data || !buf)
            return 0;

        auto* dst = static_cast<char*>(data);
        size_t pos = 0;

        // Invariant: pos <= bufSize, so `bufSize - pos` never wraps.
        while (pos < bufSize)
        {
            if (bufSize - pos < 3)
                return 0;
            const uint16_t fieldIndex = static_cast<uint16_t>(buf[pos]) | (static_cast<uint16_t>(buf[pos + 1]) << 8);
            const auto typeTag = static_cast<FieldType>(buf[pos + 2]);
            pos += 3;

            const FieldInfo* field = nullptr;
            if (fieldIndex < typeInfo.fields.size())
            {
                const FieldInfo& candidate = typeInfo.fields[fieldIndex];
                if (candidate.serialized && candidate.type == typeTag)
                    field = &candidate;
            }

            if (typeTag == FieldType::String)
            {
                if (bufSize - pos < 4)
                    return 0;
                const uint32_t len = static_cast<uint32_t>(buf[pos]) | (static_cast<uint32_t>(buf[pos + 1]) << 8) |
                                     (static_cast<uint32_t>(buf[pos + 2]) << 16) |
                                     (static_cast<uint32_t>(buf[pos + 3]) << 24);
                pos += 4;
                if (len > bufSize - pos)
                    return 0;

                if (field)
                {
                    auto* str = reinterpret_cast<std::string*>(dst + field->offset);
                    str->assign(reinterpret_cast<const char*>(buf + pos), len);
                }
                pos += len;
            }
            else
            {
                if (bufSize - pos < 2)
                    return 0;
                const uint16_t sz = static_cast<uint16_t>(buf[pos]) | (static_cast<uint16_t>(buf[pos + 1]) << 8);
                pos += 2;
                if (sz > bufSize - pos)
                    return 0;

                if (field && IsBinaryPlainFieldType(field->type) && field->size == sz)
                {
                    if (field->type == FieldType::Bool)
                    {
                        // Any non-zero byte means true; never store a bool object
                        // representation other than 0 or 1.
                        bool value = false;
                        for (uint16_t i = 0; i < sz; ++i)
                            value = value || buf[pos + i] != 0;
                        *reinterpret_cast<bool*>(dst + field->offset) = value;
                    }
                    else
                    {
                        std::memcpy(dst + field->offset, buf + pos, sz);
                    }
                }
                pos += sz;
            }
        }

        return pos;
    }

    /**
     * @brief Convenience: serialize a type looked up by name.
     * @return Empty map if type not found in registry.
     */
    inline std::unordered_map<std::string, std::string> SerializeByName(const void* data, const std::string& typeName)
    {
        const auto* typeInfo = TypeRegistry::Get().FindTypeByName(typeName);
        if (!typeInfo)
            return {};
        return SerializeToProperties(data, *typeInfo);
    }

    /**
     * @brief Convenience: deserialize into a type looked up by name.
     * @return false if type not found in registry.
     */
    inline bool DeserializeByName(void* data, const std::string& typeName,
                                  const std::unordered_map<std::string, std::string>& props)
    {
        const auto* typeInfo = TypeRegistry::Get().FindTypeByName(typeName);
        if (!typeInfo)
            return false;
        DeserializeFromProperties(data, *typeInfo, props);
        return true;
    }

} // namespace Spark
