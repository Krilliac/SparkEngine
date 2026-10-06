/**
 * @file SceneJSONReader.cpp
 * @brief JSON scene document decoder (moved out of JSONSceneSerializer.cpp)
 *
 * Contains the dependency-free JSON parser the editor scene format uses, the field
 * readers over its value tree, the schema-tagged component payload reader, and
 * DecodeSceneJSONDocument, the body SceneSerializer::LoadJSON runs on a file's bytes.
 */

#include "SceneJSONReader.h"

#include "SceneComponentCodec.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <locale>
#include <span>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace SparkEditor
{

    bool IsValidSceneUTF8(std::string_view value)
    {
        size_t i = 0;
        const auto continuation = [&](size_t offset)
        { return offset < value.size() && (static_cast<unsigned char>(value[offset]) & 0xc0) == 0x80; };
        while (i < value.size())
        {
            const auto c = static_cast<unsigned char>(value[i]);
            if (c <= 0x7f)
            {
                ++i;
                continue;
            }
            if (c >= 0xc2 && c <= 0xdf && continuation(i + 1))
            {
                i += 2;
                continue;
            }
            if (c == 0xe0 && i + 2 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0xa0 &&
                static_cast<unsigned char>(value[i + 1]) <= 0xbf && continuation(i + 2))
            {
                i += 3;
                continue;
            }
            if (((c >= 0xe1 && c <= 0xec) || (c >= 0xee && c <= 0xef)) && continuation(i + 1) && continuation(i + 2))
            {
                i += 3;
                continue;
            }
            if (c == 0xed && i + 2 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
                static_cast<unsigned char>(value[i + 1]) <= 0x9f && continuation(i + 2))
            {
                i += 3;
                continue;
            }
            if (c == 0xf0 && i + 3 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x90 &&
                static_cast<unsigned char>(value[i + 1]) <= 0xbf && continuation(i + 2) && continuation(i + 3))
            {
                i += 4;
                continue;
            }
            if (c >= 0xf1 && c <= 0xf3 && continuation(i + 1) && continuation(i + 2) && continuation(i + 3))
            {
                i += 4;
                continue;
            }
            if (c == 0xf4 && i + 3 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
                static_cast<unsigned char>(value[i + 1]) <= 0x8f && continuation(i + 2) && continuation(i + 3))
            {
                i += 4;
                continue;
            }
            return false;
        }
        return true;
    }

    bool AppendSceneValidation(const SceneFile& scene, SerializationResult& result)
    {
        std::vector<std::string> errors;
        const bool valid = scene.Validate(errors);
        for (const auto& err : errors)
        {
            result.warnings.push_back(err);
        }
        return valid;
    }

    // =============================================================================
    // Simple JSON Parser (no external dependencies)
    // =============================================================================

    namespace
    {

        // Forward declarations. std::vector does not require a complete type
        // at the point of member declaration, only when member functions are
        // instantiated, so the ordering below is valid.
        struct JSONMember;
        using JSONObject = std::vector<JSONMember>;

        struct JSONValue
        {
            enum Type : std::uint8_t
            {
                NONE,
                STRING,
                NUMBER,
                BOOL,
                OBJECT,
                ARRAY
            } type = NONE;
            std::string strVal;
            std::string numText;
            double numVal = 0.0;
            bool boolVal = false;
            JSONObject objVal;
            std::vector<JSONValue> arrVal;

            std::string GetString(const std::string& def = "") const { return type == STRING ? strVal : def; }
            float GetFloat(float def = 0.0f) const
            {
                float value = 0.0f;
                return TryGetFloat(value) ? value : def;
            }
            bool TryGetFloat(float& value) const
            {
                if (type != NUMBER || numVal < -std::numeric_limits<float>::max() ||
                    numVal > std::numeric_limits<float>::max())
                {
                    return false;
                }
                value = static_cast<float>(numVal);
                return true;
            }
            bool TryGetInt(int& value) const
            {
                if (type != NUMBER || numText.empty())
                {
                    return false;
                }
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            bool TryGetInt64(int64_t& value) const
            {
                if (type != NUMBER || numText.empty())
                {
                    return false;
                }
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            bool TryGetUint32(uint32_t& value) const
            {
                if (type != NUMBER || numText.empty())
                {
                    return false;
                }
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            uint32_t GetUint32(uint32_t def = 0) const
            {
                uint32_t value = 0;
                return TryGetUint32(value) ? value : def;
            }
            bool TryGetUint64(uint64_t& value) const
            {
                if (type != NUMBER || numText.empty())
                {
                    return false;
                }
                const auto result = std::from_chars(numText.data(), numText.data() + numText.size(), value);
                return result.ec == std::errc{} && result.ptr == numText.data() + numText.size();
            }
            uint64_t GetUint64(uint64_t def = 0) const
            {
                uint64_t value = 0;
                return TryGetUint64(value) ? value : def;
            }
            bool GetBool(bool def = false) const { return type == BOOL ? boolVal : def; }

            // Defined out-of-line after JSONMember is complete.
            const JSONValue* Find(const std::string& key) const;

            XMFLOAT3 GetFloat3(XMFLOAT3 def = {0, 0, 0}) const
            {
                if (type != ARRAY || arrVal.size() < 3)
                {
                    return def;
                }
                return {arrVal[0].GetFloat(), arrVal[1].GetFloat(), arrVal[2].GetFloat()};
            }

            XMFLOAT4 GetFloat4(XMFLOAT4 def = {0, 0, 0, 1}) const
            {
                if (type != ARRAY || arrVal.size() < 4)
                {
                    return def;
                }
                return {arrVal[0].GetFloat(), arrVal[1].GetFloat(), arrVal[2].GetFloat(), arrVal[3].GetFloat()};
            }
        };

        // Plain struct instead of std::pair to avoid Clang + libstdc++ 14
        // constructibility trait checks on incomplete types.
        struct JSONMember
        {
            std::string key;
            JSONValue value;
        };

        const JSONValue* JSONValue::Find(const std::string& key) const
        {
            if (type != OBJECT)
            {
                return nullptr;
            }
            for (const auto& m : objVal)
            {
                if (m.key == key)
                {
                    return &m.value;
                }
            }
            return nullptr;
        }

        class JSONParser
        {
          public:
            explicit JSONParser(const std::string& input) : m_input(input) {}

            bool Parse(JSONValue& out)
            {
                SkipWhitespace();
                if (!ParseValue(out))
                {
                    return false;
                }
                SkipWhitespace();
                return m_pos == m_input.size();
            }

          private:
            class DepthScope
            {
              public:
                explicit DepthScope(JSONParser& parser) : m_parser(parser), m_entered(parser.EnterContainer()) {}
                ~DepthScope()
                {
                    if (m_entered)
                    {
                        --m_parser.m_depth;
                    }
                }
                explicit operator bool() const { return m_entered; }

              private:
                JSONParser& m_parser;
                bool m_entered;
            };

            bool EnterContainer()
            {
                constexpr size_t kMaxDepth = 128;
                if (m_depth >= kMaxDepth)
                {
                    return false;
                }
                ++m_depth;
                return true;
            }

            bool ReserveNode()
            {
                constexpr size_t kMaxNodes = 250'000;
                if (m_nodeCount >= kMaxNodes)
                {
                    return false;
                }
                ++m_nodeCount;
                return true;
            }

            void SkipWhitespace()
            {
                while (m_pos < m_input.size() && std::isspace(static_cast<unsigned char>(m_input[m_pos])))
                {
                    m_pos++;
                }
            }

            char Peek() { return m_pos < m_input.size() ? m_input[m_pos] : '\0'; }
            char Next() { return m_pos < m_input.size() ? m_input[m_pos++] : '\0'; }

            bool ParseValue(JSONValue& val)
            {
                if (!ReserveNode())
                {
                    return false;
                }
                SkipWhitespace();
                const char c = Peek();
                if (c == '"')
                {
                    return ParseString(val);
                }
                if (c == '{')
                {
                    return ParseObject(val);
                }
                if (c == '[')
                {
                    return ParseArray(val);
                }
                if (c == 't' || c == 'f')
                {
                    return ParseBool(val);
                }
                if (c == 'n')
                {
                    return ParseNull(val);
                }
                if (c == '-' || std::isdigit(static_cast<unsigned char>(c)))
                {
                    return ParseNumber(val);
                }
                return false;
            }

            bool ParseString(JSONValue& val)
            {
                if (Next() != '"')
                {
                    return false;
                }
                std::string s;
                while (m_pos < m_input.size())
                {
                    const char c = Next();
                    if (c == '"')
                    {
                        if (!IsValidSceneUTF8(s))
                        {
                            return false;
                        }
                        val.type = JSONValue::STRING;
                        val.strVal = s;
                        return true;
                    }
                    if (c == '\\')
                    {
                        if (m_pos >= m_input.size())
                        {
                            return false;
                        }
                        const char esc = Next();
                        switch (esc)
                        {
                        case '"':
                            s += '"';
                            break;
                        case '\\':
                            s += '\\';
                            break;
                        case '/':
                            s += '/';
                            break;
                        case 'b':
                            s += '\b';
                            break;
                        case 'f':
                            s += '\f';
                            break;
                        case 'n':
                            s += '\n';
                            break;
                        case 'r':
                            s += '\r';
                            break;
                        case 't':
                            s += '\t';
                            break;
                        case 'u':
                        {
                            uint32_t codePoint = 0;
                            if (!ParseHexCodeUnit(codePoint))
                            {
                                return false;
                            }
                            if (codePoint >= 0xd800 && codePoint <= 0xdbff)
                            {
                                if (m_pos + 2 > m_input.size() || m_input[m_pos] != '\\' || m_input[m_pos + 1] != 'u')
                                {
                                    return false;
                                }
                                m_pos += 2;
                                uint32_t low = 0;
                                if (!ParseHexCodeUnit(low) || low < 0xdc00 || low > 0xdfff)
                                {
                                    return false;
                                }
                                codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
                            }
                            else if (codePoint >= 0xdc00 && codePoint <= 0xdfff)
                            {
                                return false;
                            }
                            AppendUTF8(s, codePoint);
                            break;
                        }
                        default:
                            return false;
                        }
                    }
                    else
                    {
                        if (static_cast<unsigned char>(c) < 0x20)
                        {
                            return false;
                        }
                        s += c;
                    }
                }
                return false;
            }

            bool ParseHexCodeUnit(uint32_t& value)
            {
                if (m_pos + 4 > m_input.size())
                {
                    return false;
                }
                value = 0;
                for (int i = 0; i < 4; ++i)
                {
                    const auto c = static_cast<unsigned char>(m_input[m_pos++]);
                    value <<= 4;
                    if (c >= '0' && c <= '9')
                    {
                        value |= c - '0';
                    }
                    else if (c >= 'a' && c <= 'f')
                    {
                        value |= c - 'a' + 10;
                    }
                    else if (c >= 'A' && c <= 'F')
                    {
                        value |= c - 'A' + 10;
                    }
                    else
                    {
                        return false;
                    }
                }
                return true;
            }

            static void AppendUTF8(std::string& output, uint32_t codePoint)
            {
                if (codePoint <= 0x7f)
                {
                    output += static_cast<char>(codePoint);
                }
                else if (codePoint <= 0x7ff)
                {
                    output += static_cast<char>(0xc0 | (codePoint >> 6));
                    output += static_cast<char>(0x80 | (codePoint & 0x3f));
                }
                else if (codePoint <= 0xffff)
                {
                    output += static_cast<char>(0xe0 | (codePoint >> 12));
                    output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
                    output += static_cast<char>(0x80 | (codePoint & 0x3f));
                }
                else
                {
                    output += static_cast<char>(0xf0 | (codePoint >> 18));
                    output += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
                    output += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
                    output += static_cast<char>(0x80 | (codePoint & 0x3f));
                }
            }

            bool ParseNumber(JSONValue& val)
            {
                const size_t start = m_pos;
                if (Peek() == '-')
                {
                    m_pos++;
                }
                if (m_pos >= m_input.size() || !std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                {
                    return false;
                }
                if (m_input[m_pos] == '0')
                {
                    ++m_pos;
                }
                else
                {
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                    {
                        m_pos++;
                    }
                }
                if (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                {
                    return false;
                }
                if (m_pos < m_input.size() && m_input[m_pos] == '.')
                {
                    m_pos++;
                    const size_t fractionalStart = m_pos;
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                    {
                        m_pos++;
                    }
                    if (m_pos == fractionalStart)
                    {
                        return false;
                    }
                }
                if (m_pos < m_input.size() && (m_input[m_pos] == 'e' || m_input[m_pos] == 'E'))
                {
                    m_pos++;
                    if (m_pos < m_input.size() && (m_input[m_pos] == '+' || m_input[m_pos] == '-'))
                    {
                        m_pos++;
                    }
                    const size_t exponentStart = m_pos;
                    while (m_pos < m_input.size() && std::isdigit(static_cast<unsigned char>(m_input[m_pos])))
                    {
                        m_pos++;
                    }
                    if (m_pos == exponentStart)
                    {
                        return false;
                    }
                }
                val.numText = m_input.substr(start, m_pos - start);
                // libc++ versions used by the MSan runner do not provide the
                // floating-point std::from_chars overload. A classic-locale,
                // no-skip stream keeps JSON parsing locale-independent while
                // remaining portable across every supported standard library.
                std::istringstream numberStream(val.numText);
                numberStream.imbue(std::locale::classic());
                numberStream >> std::noskipws >> val.numVal;
                if (numberStream.fail() || numberStream.peek() != std::char_traits<char>::eof())
                {
                    return false;
                }
                if (!std::isfinite(val.numVal))
                {
                    return false;
                }
                val.type = JSONValue::NUMBER;
                return true;
            }

            bool ParseBool(JSONValue& val)
            {
                if (m_input.compare(m_pos, 4, "true") == 0)
                {
                    m_pos += 4;
                    val.type = JSONValue::BOOL;
                    val.boolVal = true;
                    return true;
                }
                if (m_input.compare(m_pos, 5, "false") == 0)
                {
                    m_pos += 5;
                    val.type = JSONValue::BOOL;
                    val.boolVal = false;
                    return true;
                }
                return false;
            }

            bool ParseNull(JSONValue& val)
            {
                if (m_input.compare(m_pos, 4, "null") == 0)
                {
                    m_pos += 4;
                    val.type = JSONValue::NONE;
                    return true;
                }
                return false;
            }

            bool ParseObject(JSONValue& val)
            {
                if (Next() != '{')
                {
                    return false;
                }
                DepthScope depth(*this);
                if (!depth)
                {
                    return false;
                }
                val.type = JSONValue::OBJECT;
                SkipWhitespace();
                if (Peek() == '}')
                {
                    m_pos++;
                    return true;
                }
                while (true)
                {
                    SkipWhitespace();
                    JSONValue key;
                    if (!ReserveNode() || !ParseString(key))
                    {
                        return false;
                    }
                    SkipWhitespace();
                    if (Next() != ':')
                    {
                        return false;
                    }
                    JSONValue value;
                    if (!ParseValue(value))
                    {
                        return false;
                    }
                    val.objVal.push_back(JSONMember{key.strVal, std::move(value)});
                    SkipWhitespace();
                    const char c = Next();
                    if (c == '}')
                    {
                        return true;
                    }
                    if (c != ',')
                    {
                        return false;
                    }
                }
            }

            bool ParseArray(JSONValue& val)
            {
                if (Next() != '[')
                {
                    return false;
                }
                DepthScope depth(*this);
                if (!depth)
                {
                    return false;
                }
                val.type = JSONValue::ARRAY;
                SkipWhitespace();
                if (Peek() == ']')
                {
                    m_pos++;
                    return true;
                }
                while (true)
                {
                    JSONValue elem;
                    if (!ParseValue(elem))
                    {
                        return false;
                    }
                    val.arrVal.push_back(std::move(elem));
                    SkipWhitespace();
                    const char c = Next();
                    if (c == ']')
                    {
                        return true;
                    }
                    if (c != ',')
                    {
                        return false;
                    }
                }
            }

            const std::string& m_input;
            size_t m_pos = 0;
            size_t m_depth = 0;
            size_t m_nodeCount = 0;
        };

        // Helper to read a field safely
        const JSONValue* Field(const JSONValue& obj, const std::string& key)
        {
            return obj.Find(key);
        }

        bool OptionalType(const JSONValue& object, const char* key, JSONValue::Type type)
        {
            const JSONValue* value = object.Find(key);
            return !value || value->type == type;
        }

        bool OptionalFloat(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
            {
                return true;
            }
            float parsed = 0.0f;
            return value->TryGetFloat(parsed);
        }

        bool OptionalInt(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
            {
                return true;
            }
            int parsed = 0;
            return value->TryGetInt(parsed);
        }

        bool OptionalUint32(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
            {
                return true;
            }
            uint32_t parsed = 0;
            return value->TryGetUint32(parsed);
        }

        bool OptionalUint64(const JSONValue& object, const char* key)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
            {
                return true;
            }
            uint64_t parsed = 0;
            return value->TryGetUint64(parsed);
        }

        bool OptionalFloatVector(const JSONValue& object, const char* key, size_t length)
        {
            const JSONValue* value = object.Find(key);
            if (!value)
            {
                return true;
            }
            if (value->type != JSONValue::ARRAY || value->arrVal.size() != length)
            {
                return false;
            }
            for (const JSONValue& element : value->arrVal)
            {
                float parsed = 0.0f;
                if (!element.TryGetFloat(parsed))
                {
                    return false;
                }
            }
            return true;
        }

        std::string FieldStr(const JSONValue& obj, const std::string& key, const std::string& def = "")
        {
            const auto* v = obj.Find(key);
            return v ? v->GetString(def) : def;
        }
        float FieldFloat(const JSONValue& obj, const std::string& key, float def = 0.0f)
        {
            const auto* v = obj.Find(key);
            return v ? v->GetFloat(def) : def;
        }
        bool FieldIntChecked(const JSONValue& obj, const std::string& key, int def, int& value)
        {
            const auto* v = obj.Find(key);
            if (!v)
            {
                value = def;
                return true;
            }
            return v->TryGetInt(value);
        }
        uint32_t FieldUint32(const JSONValue& obj, const std::string& key, uint32_t def = 0)
        {
            const auto* v = obj.Find(key);
            return v ? v->GetUint32(def) : def;
        }
        uint64_t FieldUint64(const JSONValue& obj, const std::string& key, uint64_t def = 0)
        {
            const auto* v = obj.Find(key);
            return v ? v->GetUint64(def) : def;
        }
        bool FieldBool(const JSONValue& obj, const std::string& key, bool def = false)
        {
            const auto* v = obj.Find(key);
            return v ? v->GetBool(def) : def;
        }
        XMFLOAT3 FieldFloat3(const JSONValue& obj, const std::string& key, XMFLOAT3 def = {0, 0, 0})
        {
            const auto* v = obj.Find(key);
            return v ? v->GetFloat3(def) : def;
        }
        XMFLOAT4 FieldFloat4(const JSONValue& obj, const std::string& key, XMFLOAT4 def = {0, 0, 0, 1})
        {
            const auto* v = obj.Find(key);
            return v ? v->GetFloat4(def) : def;
        }

        std::string ComponentTypeToString(ComponentType type)
        {
            const char* name = SceneComponentTypeName(type);
            return name ? name : std::string{};
        }

        class JSONSceneComponentReader final : public SceneComponentFieldReader
        {
          public:
            explicit JSONSceneComponentReader(const JSONValue& fields) : m_fields(fields) {}

            bool HasExactly(std::span<const std::string_view> names) const override
            {
                if (m_fields.type != JSONValue::OBJECT || m_fields.objVal.size() != names.size())
                {
                    return false;
                }
                for (std::string_view name : names)
                {
                    size_t matches = 0;
                    for (const JSONMember& member : m_fields.objVal)
                    {
                        matches += member.key == name ? 1u : 0u;
                    }
                    if (matches != 1)
                    {
                        return false;
                    }
                }
                return true;
            }

            bool ReadBool(std::string_view name, bool& value) const override
            {
                const JSONValue* field = Find(name);
                if (!field || field->type != JSONValue::BOOL)
                {
                    return false;
                }
                value = field->boolVal;
                return true;
            }
            bool ReadSigned(std::string_view name, int64_t& value) const override
            {
                const JSONValue* field = Find(name);
                return field && field->TryGetInt64(value);
            }
            bool ReadUnsigned(std::string_view name, uint64_t& value) const override
            {
                const JSONValue* field = Find(name);
                return field && field->TryGetUint64(value);
            }
            bool ReadFloat(std::string_view name, float& value) const override
            {
                const JSONValue* field = Find(name);
                return field && field->TryGetFloat(value);
            }
            bool ReadString(std::string_view name, std::string& value) const override
            {
                const JSONValue* field = Find(name);
                constexpr size_t kMaxComponentStringBytes = size_t{1024} * 1024;
                if (!field || field->type != JSONValue::STRING || field->strVal.size() > kMaxComponentStringBytes ||
                    field->strVal.find('\0') != std::string::npos)
                {
                    return false;
                }
                value = field->strVal;
                return true;
            }
            bool ReadFloat2(std::string_view name, XMFLOAT2& value) const override
            {
                float parsed[2]{};
                if (!ReadFloatArray(name, parsed))
                {
                    return false;
                }
                value = {parsed[0], parsed[1]};
                return true;
            }
            bool ReadFloat3(std::string_view name, XMFLOAT3& value) const override
            {
                float parsed[3]{};
                if (!ReadFloatArray(name, parsed))
                {
                    return false;
                }
                value = {parsed[0], parsed[1], parsed[2]};
                return true;
            }
            bool ReadFloat4(std::string_view name, XMFLOAT4& value) const override
            {
                float parsed[4]{};
                if (!ReadFloatArray(name, parsed))
                {
                    return false;
                }
                value = {parsed[0], parsed[1], parsed[2], parsed[3]};
                return true;
            }

          private:
            const JSONValue* Find(std::string_view name) const { return m_fields.Find(std::string(name)); }

            template <size_t Size> bool ReadFloatArray(std::string_view name, float (&values)[Size]) const
            {
                const JSONValue* field = Find(name);
                if (!field || field->type != JSONValue::ARRAY || field->arrVal.size() != Size)
                {
                    return false;
                }
                for (size_t index = 0; index < Size; ++index)
                {
                    if (!field->arrVal[index].TryGetFloat(values[index]))
                    {
                        return false;
                    }
                }
                return true;
            }

            const JSONValue& m_fields;
        };

        /// N and N-1 are readable (OD-03); N-1 is migrated in memory with a warning.
        bool ApplySceneFileVersionWindow(uint32_t fileVersion, SceneFile& scene, SerializationResult& result)
        {
            if (fileVersion < SCENE_FILE_OLDEST_READABLE_VERSION || fileVersion > SCENE_FILE_VERSION)
            {
                const std::string window =
                    "this build reads scene versions " + std::to_string(SCENE_FILE_OLDEST_READABLE_VERSION) + "-" +
                    std::to_string(SCENE_FILE_VERSION) + " and writes version " + std::to_string(SCENE_FILE_VERSION);
                result.errorMessage =
                    "Scene file version " + std::to_string(fileVersion) + " is unsupported: " + window +
                    (fileVersion > SCENE_FILE_VERSION ? "; open it with the newer SparkEngine build that wrote it"
                                                      : "; convert it with an older build that reads version " +
                                                            std::to_string(fileVersion) + ", then resave");
                return false;
            }

            if (fileVersion < SCENE_FILE_VERSION)
            {
                // v1 -> v2 keeps the document structure; the only v2 change is the
                // schema-tagged component payload, which the loader enforces per
                // component (v1 raw object images are rejected there, never decoded).
                scene.header.version = SCENE_FILE_VERSION;
                result.warnings.push_back("Scene migrated in memory from version " + std::to_string(fileVersion) +
                                          " to " + std::to_string(SCENE_FILE_VERSION) +
                                          "; the file on disk is unchanged until it is saved");
            }
            return true;
        }

        bool DecodeObjects(const JSONValue& root, SceneFile& loadedScene, SerializationResult& result)
        {
            const auto* objectsArr = Field(root, "objects");
            if (objectsArr && objectsArr->type != JSONValue::ARRAY)
            {
                result.errorMessage = "Scene objects must be an array";
                return false;
            }
            if (!objectsArr)
            {
                return true;
            }
            for (const auto& objVal : objectsArr->arrVal)
            {
                if (objVal.type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Every scene object entry must be an object";
                    return false;
                }
                if (!OptionalUint64(objVal, "id") || !OptionalType(objVal, "name", JSONValue::STRING) ||
                    !OptionalType(objVal, "tag", JSONValue::STRING) || !OptionalInt(objVal, "layer") ||
                    !OptionalType(objVal, "active", JSONValue::BOOL) ||
                    !OptionalType(objVal, "staticObject", JSONValue::BOOL) ||
                    !OptionalFloatVector(objVal, "position", 3))
                {
                    result.errorMessage = "Scene object fields have invalid JSON types or ranges";
                    return false;
                }
                SceneObject obj;
                obj.id = FieldUint64(objVal, "id", INVALID_OBJECT_ID);
                obj.name = FieldStr(objVal, "name", "GameObject");
                obj.tag = FieldStr(objVal, "tag", "Default");
                if (!FieldIntChecked(objVal, "layer", 0, obj.layer))
                {
                    result.errorMessage = "Object layer must be an in-range integer";
                    return false;
                }
                obj.active = FieldBool(objVal, "active", true);
                obj.staticObject = FieldBool(objVal, "staticObject", false);

                // Transform
                const auto* txVal = Field(objVal, "transform");
                if (txVal && txVal->type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Object transform must be an object";
                    return false;
                }
                if (txVal && txVal->type == JSONValue::OBJECT)
                {
                    if (!OptionalFloatVector(*txVal, "position", 3) || !OptionalFloatVector(*txVal, "rotation", 4) ||
                        !OptionalFloatVector(*txVal, "scale", 3) || !OptionalUint64(*txVal, "parentID"))
                    {
                        result.errorMessage = "Object transform fields have invalid JSON types or ranges";
                        return false;
                    }
                    obj.transform.position = FieldFloat3(*txVal, "position");
                    obj.transform.rotation = FieldFloat4(*txVal, "rotation", {0, 0, 0, 1});
                    obj.transform.scale = FieldFloat3(*txVal, "scale", {1, 1, 1});
                    obj.transform.parentID = FieldUint64(*txVal, "parentID", INVALID_OBJECT_ID);

                    const auto* childArr = Field(*txVal, "childIDs");
                    if (childArr && childArr->type == JSONValue::ARRAY)
                    {
                        for (const auto& cid : childArr->arrVal)
                        {
                            uint64_t childID = 0;
                            if (!cid.TryGetUint64(childID))
                            {
                                result.errorMessage = "Object child IDs must be unsigned integers";
                                return false;
                            }
                            obj.transform.childIDs.push_back(childID);
                        }
                    }
                    else if (childArr)
                    {
                        result.errorMessage = "Object childIDs must be an array";
                        return false;
                    }
                }
                else
                {
                    // Legacy format: position as top-level array
                    const auto* posArr = Field(objVal, "position");
                    if (posArr)
                    {
                        obj.transform.position = posArr->GetFloat3();
                    }
                }

                // Component types
                const auto* ctArr = Field(objVal, "componentTypes");
                if (ctArr && ctArr->type == JSONValue::ARRAY)
                {
                    for (const auto& ct : ctArr->arrVal)
                    {
                        if (ct.type != JSONValue::STRING)
                        {
                            result.errorMessage = "Object component types must be strings";
                            return false;
                        }
                        ComponentType componentType = ComponentType::CUSTOM;
                        if (!TryParseSceneComponentTypeName(ct.GetString(), componentType))
                        {
                            result.errorMessage = "Unknown scene object component type";
                            return false;
                        }
                        obj.componentTypes.push_back(componentType);
                    }
                }
                else if (ctArr)
                {
                    result.errorMessage = "Object componentTypes must be an array";
                    return false;
                }

                loadedScene.objects.push_back(std::move(obj));
            }
            return true;
        }

        bool DecodeComponents(const JSONValue& root, uint32_t sourceVersion, SceneFile& loadedScene,
                              SerializationResult& result)
        {
            const auto* compsArr = Field(root, "components");
            if (compsArr && compsArr->type != JSONValue::ARRAY)
            {
                result.errorMessage = "Scene components must be an array";
                return false;
            }
            if (!compsArr)
            {
                return true;
            }
            for (const auto& compVal : compsArr->arrVal)
            {
                if (compVal.type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Every scene component entry must be an object";
                    return false;
                }
                if (!OptionalType(compVal, "type", JSONValue::STRING) || !OptionalUint64(compVal, "objectID") ||
                    !OptionalType(compVal, "enabled", JSONValue::BOOL))
                {
                    result.errorMessage = "Scene component fields have invalid JSON types or ranges";
                    return false;
                }
                Component comp;
                if (!TryParseSceneComponentTypeName(FieldStr(compVal, "type"), comp.type))
                {
                    result.errorMessage = "Unknown serialized component type";
                    return false;
                }
                comp.objectID = FieldUint64(compVal, "objectID");
                comp.enabled = FieldBool(compVal, "enabled", true);

                const JSONValue* data = Field(compVal, "data");
                const bool markerOnly =
                    comp.type == ComponentType::TRANSFORM || comp.type == ComponentType::SPRITE_ANIMATOR;
                if (sourceVersion < SCENE_FILE_VERSION && (data || !markerOnly))
                {
                    // v1 stored components as hex-encoded raw C++ object images
                    // (including padding and pointer-bearing members). They have
                    // no schema, so decoding them would trust arbitrary bytes.
                    result.errorMessage = "Scene file version " + std::to_string(sourceVersion) + " component " +
                                          ComponentTypeToString(comp.type) +
                                          " carries a raw object-image payload that cannot be migrated to version " +
                                          std::to_string(SCENE_FILE_VERSION) +
                                          "; remove the component or recreate it, then resave";
                    return false;
                }
                if (markerOnly)
                {
                    if (data)
                    {
                        result.errorMessage = "Marker-only scene component must not contain data";
                        return false;
                    }
                    loadedScene.components.push_back(std::move(comp));
                    continue;
                }
                if (!HasSceneComponentPayloadCodec(comp.type) || !data || data->type != JSONValue::OBJECT ||
                    data->objVal.size() != 2)
                {
                    result.errorMessage = "Scene component requires a registered schema-tagged data object";
                    return false;
                }

                const JSONValue* schema = data->Find("schema");
                const JSONValue* fields = data->Find("fields");
                uint32_t schemaVersion = 0;
                size_t schemaKeys = 0;
                size_t fieldKeys = 0;
                for (const JSONMember& member : data->objVal)
                {
                    schemaKeys += member.key == "schema" ? 1u : 0u;
                    fieldKeys += member.key == "fields" ? 1u : 0u;
                }
                if (schemaKeys != 1 || fieldKeys != 1 || !schema || !schema->TryGetUint32(schemaVersion) ||
                    schemaVersion != SCENE_COMPONENT_SCHEMA_VERSION || !fields || fields->type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Scene component data schema is invalid or unsupported";
                    return false;
                }

                const JSONSceneComponentReader payloadReader(*fields);
                std::string codecError;
                if (!DecodeSceneComponentPayload(comp.type, payloadReader, comp, codecError))
                {
                    result.errorMessage = "Cannot decode " + ComponentTypeToString(comp.type) + ": " + codecError;
                    return false;
                }
                loadedScene.components.push_back(std::move(comp));
            }
            return true;
        }

        bool DecodeEnvironment(const JSONValue& root, SceneFile& loadedScene, SerializationResult& result)
        {
            const auto* envVal = Field(root, "environment");
            if (envVal && envVal->type != JSONValue::OBJECT)
            {
                result.errorMessage = "Scene environment must be an object";
                return false;
            }
            if (!envVal)
            {
                return true;
            }
            const bool validEnvironment =
                OptionalInt(*envVal, "skyType") && OptionalFloatVector(*envVal, "skyColor", 4) &&
                OptionalFloatVector(*envVal, "horizonColor", 4) &&
                OptionalType(*envVal, "skyboxAssetPath", JSONValue::STRING) &&
                OptionalType(*envVal, "fogEnabled", JSONValue::BOOL) && OptionalFloatVector(*envVal, "fogColor", 4) &&
                OptionalFloat(*envVal, "fogDensity") && OptionalFloat(*envVal, "fogStart") &&
                OptionalFloat(*envVal, "fogEnd") && OptionalFloatVector(*envVal, "windDirection", 3) &&
                OptionalFloat(*envVal, "windStrength") && OptionalFloat(*envVal, "windTurbulence") &&
                OptionalType(*envVal, "bloomEnabled", JSONValue::BOOL) && OptionalFloat(*envVal, "bloomIntensity") &&
                OptionalFloat(*envVal, "bloomThreshold") &&
                OptionalType(*envVal, "tonemappingEnabled", JSONValue::BOOL) && OptionalFloat(*envVal, "exposure") &&
                OptionalFloat(*envVal, "gamma");
            if (!validEnvironment)
            {
                result.errorMessage = "Scene environment fields have invalid JSON types or ranges";
                return false;
            }
            auto& env = loadedScene.environment;
            int skyType = 0;
            if (!FieldIntChecked(*envVal, "skyType", 0, skyType) ||
                skyType < static_cast<int>(EnvironmentSettings::SOLID_COLOR) ||
                skyType > static_cast<int>(EnvironmentSettings::PROCEDURAL))
            {
                result.errorMessage = "Environment skyType must be an in-range integer";
                return false;
            }
            env.skyType = static_cast<EnvironmentSettings::SkyType>(skyType);
            env.skyColor = FieldFloat4(*envVal, "skyColor", {0.5f, 0.8f, 1.0f, 1.0f});
            env.horizonColor = FieldFloat4(*envVal, "horizonColor", {0.9f, 0.9f, 0.9f, 1.0f});
            env.skyboxAssetPath = FieldStr(*envVal, "skyboxAssetPath");
            env.fogEnabled = FieldBool(*envVal, "fogEnabled", false);
            env.fogColor = FieldFloat4(*envVal, "fogColor", {0.7f, 0.7f, 0.7f, 1.0f});
            env.fogDensity = FieldFloat(*envVal, "fogDensity", 0.01f);
            env.fogStart = FieldFloat(*envVal, "fogStart", 10.0f);
            env.fogEnd = FieldFloat(*envVal, "fogEnd", 100.0f);
            env.windDirection = FieldFloat3(*envVal, "windDirection", {1, 0, 0});
            env.windStrength = FieldFloat(*envVal, "windStrength", 1.0f);
            env.windTurbulence = FieldFloat(*envVal, "windTurbulence", 0.1f);
            env.bloomEnabled = FieldBool(*envVal, "bloomEnabled", false);
            env.bloomIntensity = FieldFloat(*envVal, "bloomIntensity", 1.0f);
            env.bloomThreshold = FieldFloat(*envVal, "bloomThreshold", 1.0f);
            env.tonemappingEnabled = FieldBool(*envVal, "tonemappingEnabled", true);
            env.exposure = FieldFloat(*envVal, "exposure", 1.0f);
            env.gamma = FieldFloat(*envVal, "gamma", 2.2f);
            return true;
        }

        bool DecodeDefaultCamera(const JSONValue& root, SceneFile& loadedScene, SerializationResult& result)
        {
            const auto* camVal = Field(root, "defaultCamera");
            if (camVal && camVal->type != JSONValue::OBJECT)
            {
                result.errorMessage = "Scene defaultCamera must be an object";
                return false;
            }
            if (!camVal)
            {
                return true;
            }
            if (!OptionalInt(*camVal, "projectionType") || !OptionalFloat(*camVal, "fieldOfView") ||
                !OptionalFloat(*camVal, "orthographicSize") || !OptionalFloat(*camVal, "nearPlane") ||
                !OptionalFloat(*camVal, "farPlane") || !OptionalFloatVector(*camVal, "clearColor", 4) ||
                !OptionalType(*camVal, "isMainCamera", JSONValue::BOOL) || !OptionalInt(*camVal, "renderTargetWidth") ||
                !OptionalInt(*camVal, "renderTargetHeight"))
            {
                result.errorMessage = "Scene camera fields have invalid JSON types or ranges";
                return false;
            }
            auto& cam = loadedScene.defaultCamera;
            int projectionType = 0;
            if (!FieldIntChecked(*camVal, "projectionType", 0, projectionType) ||
                projectionType < static_cast<int>(Camera::PERSPECTIVE) ||
                projectionType > static_cast<int>(Camera::ORTHOGRAPHIC) ||
                !FieldIntChecked(*camVal, "renderTargetWidth", 1920, cam.renderTargetWidth) ||
                !FieldIntChecked(*camVal, "renderTargetHeight", 1080, cam.renderTargetHeight))
            {
                result.errorMessage = "Camera integer fields must be in range";
                return false;
            }
            cam.projectionType = static_cast<Camera::ProjectionType>(projectionType);
            cam.fieldOfView = FieldFloat(*camVal, "fieldOfView", 75.0f);
            cam.orthographicSize = FieldFloat(*camVal, "orthographicSize", 5.0f);
            cam.nearPlane = FieldFloat(*camVal, "nearPlane", 0.1f);
            cam.farPlane = FieldFloat(*camVal, "farPlane", 1000.0f);
            cam.clearColor = FieldFloat4(*camVal, "clearColor", {0.2f, 0.3f, 0.5f, 1.0f});
            cam.isMainCamera = FieldBool(*camVal, "isMainCamera", false);
            return true;
        }

        bool DecodeAssetReferences(const JSONValue& root, SceneFile& loadedScene, SerializationResult& result)
        {
            const auto* refsArr = Field(root, "assetReferences");
            if (refsArr && refsArr->type != JSONValue::ARRAY)
            {
                result.errorMessage = "Scene assetReferences must be an array";
                return false;
            }
            if (!refsArr)
            {
                return true;
            }
            for (const auto& refVal : refsArr->arrVal)
            {
                if (refVal.type != JSONValue::OBJECT)
                {
                    result.errorMessage = "Every scene asset reference entry must be an object";
                    return false;
                }
                if (!OptionalType(refVal, "assetPath", JSONValue::STRING) ||
                    !OptionalType(refVal, "assetType", JSONValue::STRING) || !OptionalUint64(refVal, "lastModified") ||
                    !OptionalUint64(refVal, "fileSize") || !OptionalType(refVal, "checksum", JSONValue::STRING))
                {
                    result.errorMessage = "Asset reference fields have invalid JSON types or ranges";
                    return false;
                }
                AssetReference ref;
                ref.assetPath = FieldStr(refVal, "assetPath");
                ref.assetType = FieldStr(refVal, "assetType");
                ref.lastModified = FieldUint64(refVal, "lastModified");
                ref.fileSize = FieldUint64(refVal, "fileSize");
                ref.checksum = FieldStr(refVal, "checksum");
                const auto* depsArr = Field(refVal, "dependencies");
                if (depsArr && depsArr->type == JSONValue::ARRAY)
                {
                    for (const auto& dep : depsArr->arrVal)
                    {
                        if (dep.type != JSONValue::STRING)
                        {
                            result.errorMessage = "Asset dependencies must be strings";
                            return false;
                        }
                        ref.dependencies.push_back(dep.GetString());
                    }
                }
                else if (depsArr)
                {
                    result.errorMessage = "Asset dependencies must be an array";
                    return false;
                }
                loadedScene.assetReferences.push_back(std::move(ref));
            }
            return true;
        }

    } // anonymous namespace

    bool DecodeSceneJSONDocument(const std::string& content, SceneFile& outScene, SerializationResult& result)
    {
        JSONValue root;
        JSONParser parser(content);
        if (!parser.Parse(root) || root.type != JSONValue::OBJECT)
        {
            result.success = false;
            result.errorMessage = "Failed to parse JSON";
            return false;
        }

        if (!OptionalType(root, "sceneName", JSONValue::STRING) ||
            !OptionalType(root, "description", JSONValue::STRING) || !OptionalUint32(root, "version") ||
            !OptionalUint32(root, "objectCount") || !OptionalUint32(root, "componentCount") ||
            !OptionalUint32(root, "assetReferenceCount") || !OptionalUint64(root, "timestamp") ||
            !OptionalFloatVector(root, "gravity", 3) || !OptionalFloatVector(root, "ambientColor", 4) ||
            !OptionalFloat(root, "ambientIntensity"))
        {
            result.errorMessage = "Scene header fields have invalid JSON types or ranges";
            return false;
        }

        // Build a new scene and publish it only after the entire document has
        // parsed and validated. Failed loads must not append to or partially
        // overwrite the caller's live scene.
        SceneFile loadedScene;

        // Header
        {
            const std::string name = FieldStr(root, "sceneName");
            const std::string desc = FieldStr(root, "description");
            if (name.find('\0') != std::string::npos || desc.find('\0') != std::string::npos ||
                name.size() >= sizeof(loadedScene.header.sceneName) ||
                desc.size() >= sizeof(loadedScene.header.description))
            {
                result.errorMessage = "Scene name or description contains NUL or exceeds its format capacity";
                return false;
            }
            std::memcpy(loadedScene.header.sceneName, name.data(), name.size());
            loadedScene.header.sceneName[name.size()] = '\0';
            std::memcpy(loadedScene.header.description, desc.data(), desc.size());
            loadedScene.header.description[desc.size()] = '\0';
        }
        const auto versionFieldCount = static_cast<size_t>(std::count_if(
            root.objVal.begin(), root.objVal.end(), [](const JSONMember& member) { return member.key == "version"; }));
        if (versionFieldCount != 1)
        {
            result.errorMessage = "Scene file must declare exactly one \"version\" field";
            return false;
        }
        // Read N and N-1 (OD-03). ApplySceneFileVersionWindow reports a versioned
        // error for anything else and records the in-memory upgrade for N-1.
        const uint32_t sourceVersion = FieldUint32(root, "version", 0);
        if (!ApplySceneFileVersionWindow(sourceVersion, loadedScene, result))
        {
            return false;
        }
        loadedScene.header.objectCount = FieldUint32(root, "objectCount");
        loadedScene.header.componentCount = FieldUint32(root, "componentCount");
        loadedScene.header.assetReferenceCount = FieldUint32(root, "assetReferenceCount");
        loadedScene.header.timestamp = FieldUint64(root, "timestamp");
        loadedScene.header.gravity = FieldFloat3(root, "gravity", {0, -9.81f, 0});
        loadedScene.header.ambientColor = FieldFloat4(root, "ambientColor", {0.2f, 0.2f, 0.2f, 1.0f});
        loadedScene.header.ambientIntensity = FieldFloat(root, "ambientIntensity", 1.0f);
        loadedScene.header.magic = SCENE_FILE_MAGIC;

        if (!DecodeObjects(root, loadedScene, result) || !DecodeComponents(root, sourceVersion, loadedScene, result) ||
            !DecodeEnvironment(root, loadedScene, result) || !DecodeDefaultCamera(root, loadedScene, result) ||
            !DecodeAssetReferences(root, loadedScene, result))
        {
            return false;
        }

        const auto declaredCountMatches = [&](const char* fieldName, size_t actualCount)
        {
            const JSONValue* value = Field(root, fieldName);
            if (!value)
            {
                return true;
            }
            uint32_t declaredCount = 0;
            return actualCount <= std::numeric_limits<uint32_t>::max() && value->TryGetUint32(declaredCount) &&
                   declaredCount == actualCount;
        };
        if (!declaredCountMatches("objectCount", loadedScene.objects.size()) ||
            !declaredCountMatches("componentCount", loadedScene.components.size()) ||
            !declaredCountMatches("assetReferenceCount", loadedScene.assetReferences.size()))
        {
            result.errorMessage = "Scene header counts do not match the serialized arrays";
            return false;
        }

        if (!AppendSceneValidation(loadedScene, result))
        {
            result.errorMessage = "Scene data failed validation";
            return false;
        }

        loadedScene.header.objectCount = static_cast<uint32_t>(loadedScene.objects.size());
        loadedScene.header.componentCount = static_cast<uint32_t>(loadedScene.components.size());
        loadedScene.header.assetReferenceCount = static_cast<uint32_t>(loadedScene.assetReferences.size());
        outScene = std::move(loadedScene);

        result.success = true;
        result.bytesProcessed = content.size();
        return true;
    }

} // namespace SparkEditor
