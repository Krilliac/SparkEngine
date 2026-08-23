/**
 * @file SceneComponentCodec.h
 * @brief Portable, schema-versioned persistence boundary for editor components.
 */
#pragma once

#include "SceneFileTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace SparkEditor
{
    constexpr uint32_t SCENE_COMPONENT_SCHEMA_VERSION = 1;

    class SceneComponentFieldWriter
    {
      public:
        virtual ~SceneComponentFieldWriter() = default;
        virtual bool WriteBool(std::string_view name, bool value) = 0;
        virtual bool WriteSigned(std::string_view name, int64_t value) = 0;
        virtual bool WriteUnsigned(std::string_view name, uint64_t value) = 0;
        virtual bool WriteFloat(std::string_view name, float value) = 0;
        virtual bool WriteString(std::string_view name, std::string_view value) = 0;
        virtual bool WriteFloat2(std::string_view name, const XMFLOAT2& value) = 0;
        virtual bool WriteFloat3(std::string_view name, const XMFLOAT3& value) = 0;
        virtual bool WriteFloat4(std::string_view name, const XMFLOAT4& value) = 0;

        bool Write(std::string_view name, bool value) { return WriteBool(name, value); }
        bool Write(std::string_view name, int value) { return WriteSigned(name, value); }
        bool Write(std::string_view name, uint32_t value) { return WriteUnsigned(name, value); }
        bool Write(std::string_view name, uint64_t value) { return WriteUnsigned(name, value); }
        bool Write(std::string_view name, float value) { return WriteFloat(name, value); }
        bool Write(std::string_view name, const std::string& value) { return WriteString(name, value); }
        bool Write(std::string_view name, const XMFLOAT2& value) { return WriteFloat2(name, value); }
        bool Write(std::string_view name, const XMFLOAT3& value) { return WriteFloat3(name, value); }
        bool Write(std::string_view name, const XMFLOAT4& value) { return WriteFloat4(name, value); }

        template <typename Enum>
            requires std::is_enum_v<Enum>
        bool Write(std::string_view name, Enum value)
        {
            return WriteSigned(name, static_cast<int64_t>(value));
        }

        template <size_t Size> bool Write(std::string_view name, const char (&value)[Size])
        {
            const char* end = std::find(value, value + Size, '\0');
            return end != value + Size && WriteString(name, std::string_view(value, static_cast<size_t>(end - value)));
        }
    };

    class SceneComponentFieldReader
    {
      public:
        virtual ~SceneComponentFieldReader() = default;
        virtual bool HasExactly(std::span<const std::string_view> fieldNames) const = 0;
        virtual bool ReadBool(std::string_view name, bool& value) const = 0;
        virtual bool ReadSigned(std::string_view name, int64_t& value) const = 0;
        virtual bool ReadUnsigned(std::string_view name, uint64_t& value) const = 0;
        virtual bool ReadFloat(std::string_view name, float& value) const = 0;
        virtual bool ReadString(std::string_view name, std::string& value) const = 0;
        virtual bool ReadFloat2(std::string_view name, XMFLOAT2& value) const = 0;
        virtual bool ReadFloat3(std::string_view name, XMFLOAT3& value) const = 0;
        virtual bool ReadFloat4(std::string_view name, XMFLOAT4& value) const = 0;

        bool Read(std::string_view name, bool& value) const { return ReadBool(name, value); }
        bool Read(std::string_view name, int& value) const
        {
            int64_t parsed = 0;
            if (!ReadSigned(name, parsed) || parsed < INT32_MIN || parsed > INT32_MAX)
                return false;
            value = static_cast<int>(parsed);
            return true;
        }
        bool Read(std::string_view name, uint32_t& value) const
        {
            uint64_t parsed = 0;
            if (!ReadUnsigned(name, parsed) || parsed > UINT32_MAX)
                return false;
            value = static_cast<uint32_t>(parsed);
            return true;
        }
        bool Read(std::string_view name, uint64_t& value) const { return ReadUnsigned(name, value); }
        bool Read(std::string_view name, float& value) const { return ReadFloat(name, value); }
        bool Read(std::string_view name, std::string& value) const { return ReadString(name, value); }
        bool Read(std::string_view name, XMFLOAT2& value) const { return ReadFloat2(name, value); }
        bool Read(std::string_view name, XMFLOAT3& value) const { return ReadFloat3(name, value); }
        bool Read(std::string_view name, XMFLOAT4& value) const { return ReadFloat4(name, value); }

        template <typename Enum>
            requires std::is_enum_v<Enum>
        bool Read(std::string_view name, Enum& value) const
        {
            int64_t parsed = 0;
            if (!ReadSigned(name, parsed) || parsed < INT32_MIN || parsed > INT32_MAX)
                return false;
            value = static_cast<Enum>(parsed);
            return true;
        }

        template <size_t Size> bool Read(std::string_view name, char (&value)[Size]) const
        {
            std::string parsed;
            if (!ReadString(name, parsed) || parsed.size() >= Size || parsed.find('\0') != std::string::npos)
                return false;
            std::fill(value, value + Size, '\0');
            std::copy(parsed.begin(), parsed.end(), value);
            return true;
        }
    };

    [[nodiscard]] const char* SceneComponentTypeName(ComponentType type);
    [[nodiscard]] bool TryParseSceneComponentTypeName(std::string_view name, ComponentType& type);
    [[nodiscard]] bool HasSceneComponentPayloadCodec(ComponentType type);
    [[nodiscard]] std::vector<ComponentType> GetSceneComponentPayloadTypes();
    bool InitializeDefaultSceneComponentPayload(Component& component, std::string& error);
    bool EncodeSceneComponentPayload(const Component& component, SceneComponentFieldWriter& writer, std::string& error);
    bool DecodeSceneComponentPayload(ComponentType type, const SceneComponentFieldReader& reader, Component& component,
                                     std::string& error);
} // namespace SparkEditor
