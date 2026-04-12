/**
 * @file ReflectionSerializer.h
 * @brief Generic reflection-driven serialization utilities
 *
 * Provides type-agnostic serialize/deserialize for any type registered in
 * TypeRegistry. Uses GetFieldAsString/SetFieldFromString (from Reflection.h)
 * to convert fields to/from string key-value maps or raw binary.
 *
 * Used by: SaveSystem, JSONSceneSerializer, BinarySceneSerializer,
 * EngineSettings, and any subsystem that needs generic type I/O.
 *
 * @see Core/Reflection.h, Core/ComponentReflection.cpp
 */

#pragma once

#include "Reflection.h"

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
     * @brief Serialize all reflected fields to a binary buffer.
     *
     * Format per field: [fieldIndex:uint16][typeTag:uint8][rawBytes:size]
     * Fields with serialized=false are skipped.
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
            if (!field.serialized)
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
     * Reads field headers and raw data, matching by field index.
     * Unknown or out-of-range indices are skipped (forward-compatible).
     *
     * @param data      Pointer to the struct instance.
     * @param typeInfo  Reflected type metadata.
     * @param buf       Input buffer.
     * @param bufSize   Size of the input buffer.
     * @return Number of bytes consumed, or 0 on error.
     */
    inline size_t DeserializeFromBinary(void* data, const TypeInfo& typeInfo, const uint8_t* buf, size_t bufSize)
    {
        if (!data || !buf)
            return 0;

        auto* dst = static_cast<char*>(data);
        size_t pos = 0;

        while (pos + 3 <= bufSize)
        {
            uint16_t fieldIndex = static_cast<uint16_t>(buf[pos]) | (static_cast<uint16_t>(buf[pos + 1]) << 8);
            auto typeTag = static_cast<FieldType>(buf[pos + 2]);
            pos += 3;

            if (typeTag == FieldType::String)
            {
                if (pos + 4 > bufSize)
                    break;
                uint32_t len = static_cast<uint32_t>(buf[pos]) | (static_cast<uint32_t>(buf[pos + 1]) << 8) |
                               (static_cast<uint32_t>(buf[pos + 2]) << 16) |
                               (static_cast<uint32_t>(buf[pos + 3]) << 24);
                pos += 4;
                if (pos + len > bufSize)
                    break;

                if (fieldIndex < typeInfo.fields.size())
                {
                    const auto& field = typeInfo.fields[fieldIndex];
                    if (field.type == FieldType::String)
                    {
                        auto* str = reinterpret_cast<std::string*>(dst + field.offset);
                        *str = std::string(reinterpret_cast<const char*>(buf + pos), len);
                    }
                }
                pos += len;
            }
            else
            {
                if (pos + 2 > bufSize)
                    break;
                uint16_t sz = static_cast<uint16_t>(buf[pos]) | (static_cast<uint16_t>(buf[pos + 1]) << 8);
                pos += 2;
                if (pos + sz > bufSize)
                    break;

                if (fieldIndex < typeInfo.fields.size())
                {
                    const auto& field = typeInfo.fields[fieldIndex];
                    if (field.size == sz)
                    {
                        std::memcpy(dst + field.offset, buf + pos, sz);
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
