/**
 * @file JsonUtils.h
 * @brief Minimal self-contained JSON parser and serializer
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a lightweight JSON value type with parse/stringify for config files,
 * dialogue trees, quest data, and modding definitions. No external dependencies.
 * Returns Null on malformed input instead of throwing exceptions.
 *
 * ## Usage
 * @code
 *   // Parse
 *   auto val = Spark::Json::Parse(R"({"name": "Fireball", "damage": 50})");
 *   std::string name = val["name"].AsString();   // "Fireball"
 *   double dmg = val["damage"].AsNumber();        // 50.0
 *
 *   // Build
 *   Spark::Json::Value obj;
 *   obj["level"] = Spark::Json::Value(5);
 *   obj["items"] = Spark::Json::Value::MakeArray();
 *   obj["items"].PushBack(Spark::Json::Value("Sword"));
 *
 *   // Serialize
 *   std::string json = Spark::Json::Stringify(obj);
 *   std::string pretty = Spark::Json::StringifyPretty(obj);
 * @endcode
 */

#pragma once

#include "LogMacros.h"

#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#if SPARK_HAS_NLOHMANN_JSON
#include <nlohmann_json.h>
#endif

namespace Spark::Json
{

    /**
     * @brief JSON value type discriminator.
     */
    enum class Type : uint8_t
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    /**
     * @class Value
     * @brief A JSON value that can be null, bool, number, string, array, or object.
     */
    class Value
    {
      public:
        using ArrayType = std::vector<Value>;
        using ObjectType = std::unordered_map<std::string, Value>;

        // -- Construction --

        Value() : m_data(nullptr) {}
        Value(std::nullptr_t) : m_data(nullptr) {}
        Value(bool b) : m_data(b) {}
        Value(double num) : m_data(num) {}
        Value(int num) : m_data(static_cast<double>(num)) {}
        Value(const char* str) : m_data(std::string(str)) {}
        Value(std::string str) : m_data(std::move(str)) {}

        /**
         * @brief Create an empty JSON array.
         */
        [[nodiscard]] static Value MakeArray() { return Value(ArrayType{}); }

        /**
         * @brief Create an empty JSON object.
         */
        [[nodiscard]] static Value MakeObject() { return Value(ObjectType{}); }

        // -- Type query --

        [[nodiscard]] Type GetType() const
        {
            return std::visit(
                [](const auto& val) -> Type
                {
                    using T = std::decay_t<decltype(val)>;
                    if constexpr (std::is_same_v<T, std::nullptr_t>)
                        return Type::Null;
                    else if constexpr (std::is_same_v<T, bool>)
                        return Type::Bool;
                    else if constexpr (std::is_same_v<T, double>)
                        return Type::Number;
                    else if constexpr (std::is_same_v<T, std::string>)
                        return Type::String;
                    else if constexpr (std::is_same_v<T, ArrayType>)
                        return Type::Array;
                    else
                        return Type::Object;
                },
                m_data);
        }

        [[nodiscard]] bool IsNull() const { return GetType() == Type::Null; }
        [[nodiscard]] bool IsBool() const { return GetType() == Type::Bool; }
        [[nodiscard]] bool IsNumber() const { return GetType() == Type::Number; }
        [[nodiscard]] bool IsString() const { return GetType() == Type::String; }
        [[nodiscard]] bool IsArray() const { return GetType() == Type::Array; }
        [[nodiscard]] bool IsObject() const { return GetType() == Type::Object; }

        // -- Value access (safe — logs and returns default on type mismatch) --

        [[nodiscard]] bool AsBool(bool fallback = false) const
        {
            if (auto* val = std::get_if<bool>(&m_data))
                return *val;
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value::AsBool called on non-bool (type=%d)",
                           static_cast<int>(GetType()));
            return fallback;
        }

        [[nodiscard]] double AsNumber(double fallback = 0.0) const
        {
            if (auto* val = std::get_if<double>(&m_data))
                return *val;
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value::AsNumber called on non-number (type=%d)",
                           static_cast<int>(GetType()));
            return fallback;
        }

        [[nodiscard]] int AsInt(int fallback = 0) const
        {
            if (auto* val = std::get_if<double>(&m_data))
                return static_cast<int>(*val);
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value::AsInt called on non-number (type=%d)",
                           static_cast<int>(GetType()));
            return fallback;
        }

        [[nodiscard]] const std::string& AsString() const
        {
            if (auto* val = std::get_if<std::string>(&m_data))
                return *val;
            SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value::AsString called on non-string (type=%d)",
                           static_cast<int>(GetType()));
            static const std::string s_empty;
            return s_empty;
        }

        [[nodiscard]] const std::string& AsString(const std::string& fallback) const
        {
            if (auto* val = std::get_if<std::string>(&m_data))
                return *val;
            return fallback;
        }

        // -- Array access (safe — logs and returns null on type/bounds mismatch) --

        Value& operator[](size_t index)
        {
            static Value s_null;
            auto* arr = std::get_if<ArrayType>(&m_data);
            if (!arr)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value::operator[size_t] called on non-array (type=%d)",
                               static_cast<int>(GetType()));
                s_null = Value();
                return s_null;
            }
            if (index >= arr->size())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value array index %zu out of bounds (size=%zu)", index,
                               arr->size());
                s_null = Value();
                return s_null;
            }
            return (*arr)[index];
        }

        const Value& operator[](size_t index) const
        {
            static const Value s_null;
            auto* arr = std::get_if<ArrayType>(&m_data);
            if (!arr)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "Json::Value::operator[size_t] const called on non-array (type=%d)",
                               static_cast<int>(GetType()));
                return s_null;
            }
            if (index >= arr->size())
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value array index %zu out of bounds (size=%zu)", index,
                               arr->size());
                return s_null;
            }
            return (*arr)[index];
        }

        void PushBack(Value val)
        {
            auto* arr = std::get_if<ArrayType>(&m_data);
            if (!arr)
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Core, "Json::Value::PushBack called on non-array (type=%d)",
                                static_cast<int>(GetType()));
                return;
            }
            arr->push_back(std::move(val));
        }

        [[nodiscard]] size_t Size() const
        {
            if (auto* arr = std::get_if<ArrayType>(&m_data))
                return arr->size();
            if (auto* obj = std::get_if<ObjectType>(&m_data))
                return obj->size();
            return 0;
        }

        // -- Object access --

        Value& operator[](const std::string& key)
        {
            // Auto-promote null to object on first key access
            if (IsNull())
                m_data = ObjectType{};
            auto* obj = std::get_if<ObjectType>(&m_data);
            if (!obj)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "Json::Value::operator[string] called on non-object (type=%d)",
                               static_cast<int>(GetType()));
                static Value s_null;
                s_null = Value();
                return s_null;
            }
            return (*obj)[key];
        }

        const Value& operator[](const std::string& key) const
        {
            static const Value s_null;
            auto* obj = std::get_if<ObjectType>(&m_data);
            if (!obj)
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "Json::Value::operator[string] const called on non-object (type=%d)",
                               static_cast<int>(GetType()));
                return s_null;
            }
            auto it = obj->find(key);
            if (it == obj->end())
                return s_null;
            return it->second;
        }

        [[nodiscard]] bool HasKey(const std::string& key) const
        {
            if (auto* obj = std::get_if<ObjectType>(&m_data))
                return obj->contains(key);
            return false;
        }

        [[nodiscard]] std::vector<std::string> GetKeys() const
        {
            std::vector<std::string> keys;
            if (auto* obj = std::get_if<ObjectType>(&m_data))
            {
                keys.reserve(obj->size());
                for (const auto& [k, v] : *obj)
                    keys.push_back(k);
            }
            return keys;
        }

        // -- Equality --

        [[nodiscard]] bool operator==(const Value& other) const { return m_data == other.m_data; }

      private:
        explicit Value(ArrayType arr) : m_data(std::move(arr)) {}
        explicit Value(ObjectType obj) : m_data(std::move(obj)) {}

        std::variant<std::nullptr_t, bool, double, std::string, ArrayType, ObjectType> m_data;
    };

    // =========================================================================
    // Parser (internal detail)
    // =========================================================================

    namespace Detail
    {

        class Parser
        {
          public:
            explicit Parser(std::string_view input) : m_input(input), m_pos(0) {}

            Value Parse()
            {
                SkipWhitespace();
                if (m_pos >= m_input.size())
                    return {};
                Value result = ParseValue();
                return result;
            }

          private:
            std::string_view m_input;
            size_t m_pos;

            [[nodiscard]] char Peek() const
            {
                if (m_pos >= m_input.size())
                    return '\0';
                return m_input[m_pos];
            }

            char Advance()
            {
                if (m_pos >= m_input.size())
                    return '\0';
                return m_input[m_pos++];
            }

            bool Expect(char c)
            {
                SkipWhitespace();
                if (Peek() == c)
                {
                    Advance();
                    return true;
                }
                return false;
            }

            void SkipWhitespace()
            {
                while (m_pos < m_input.size())
                {
                    char c = m_input[m_pos];
                    if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                        ++m_pos;
                    else
                        break;
                }
            }

            Value ParseValue()
            {
                SkipWhitespace();
                char c = Peek();
                if (c == '"')
                    return ParseString();
                if (c == '{')
                    return ParseObject();
                if (c == '[')
                    return ParseArray();
                if (c == 't' || c == 'f')
                    return ParseBool();
                if (c == 'n')
                    return ParseNull();
                if (c == '-' || (c >= '0' && c <= '9'))
                    return ParseNumber();
                return {}; // Malformed — return null
            }

            Value ParseNull()
            {
                if (MatchLiteral("null"))
                    return Value();
                return {};
            }

            Value ParseBool()
            {
                if (MatchLiteral("true"))
                    return Value(true);
                if (MatchLiteral("false"))
                    return Value(false);
                return {};
            }

            Value ParseNumber()
            {
                size_t start = m_pos;

                if (Peek() == '-')
                    Advance();

                // Integer part
                if (Peek() == '0')
                {
                    Advance();
                }
                else if (Peek() >= '1' && Peek() <= '9')
                {
                    while (Peek() >= '0' && Peek() <= '9')
                        Advance();
                }
                else
                {
                    return {};
                }

                // Fractional part
                if (Peek() == '.')
                {
                    Advance();
                    if (!(Peek() >= '0' && Peek() <= '9'))
                        return {};
                    while (Peek() >= '0' && Peek() <= '9')
                        Advance();
                }

                // Exponent
                if (Peek() == 'e' || Peek() == 'E')
                {
                    Advance();
                    if (Peek() == '+' || Peek() == '-')
                        Advance();
                    if (!(Peek() >= '0' && Peek() <= '9'))
                        return {};
                    while (Peek() >= '0' && Peek() <= '9')
                        Advance();
                }

                std::string numStr(m_input.substr(start, m_pos - start));
                try
                {
                    double val = std::stod(numStr);
                    return Value(val);
                }
                catch (...)
                {
                    return {};
                }
            }

            Value ParseString()
            {
                if (Advance() != '"')
                    return {};

                std::string result;
                while (m_pos < m_input.size())
                {
                    char c = Advance();
                    if (c == '"')
                        return Value(std::move(result));
                    if (c == '\\')
                    {
                        char esc = Advance();
                        switch (esc)
                        {
                        case '"':
                            result += '"';
                            break;
                        case '\\':
                            result += '\\';
                            break;
                        case '/':
                            result += '/';
                            break;
                        case 'b':
                            result += '\b';
                            break;
                        case 'f':
                            result += '\f';
                            break;
                        case 'n':
                            result += '\n';
                            break;
                        case 'r':
                            result += '\r';
                            break;
                        case 't':
                            result += '\t';
                            break;
                        case 'u':
                            result += ParseUnicodeEscape();
                            break;
                        default:
                            result += esc;
                            break;
                        }
                    }
                    else
                    {
                        result += c;
                    }
                }
                return {}; // Unterminated string
            }

            std::string ParseUnicodeEscape()
            {
                // Read 4 hex digits for BMP codepoint
                uint32_t codepoint = 0;
                for (int i = 0; i < 4; ++i)
                {
                    char c = Advance();
                    codepoint <<= 4;
                    if (c >= '0' && c <= '9')
                        codepoint |= (c - '0');
                    else if (c >= 'a' && c <= 'f')
                        codepoint |= (c - 'a' + 10);
                    else if (c >= 'A' && c <= 'F')
                        codepoint |= (c - 'A' + 10);
                    else
                        return "?"; // Invalid hex
                }

                // Simple UTF-8 encoding for BMP
                std::string result;
                if (codepoint <= 0x7F)
                {
                    result += static_cast<char>(codepoint);
                }
                else if (codepoint <= 0x7FF)
                {
                    result += static_cast<char>(0xC0 | (codepoint >> 6));
                    result += static_cast<char>(0x80 | (codepoint & 0x3F));
                }
                else
                {
                    result += static_cast<char>(0xE0 | (codepoint >> 12));
                    result += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (codepoint & 0x3F));
                }
                return result;
            }

            Value ParseArray()
            {
                Advance(); // consume '['
                SkipWhitespace();

                auto arr = Value::MakeArray();
                if (Peek() == ']')
                {
                    Advance();
                    return arr;
                }

                while (true)
                {
                    SkipWhitespace();
                    arr.PushBack(ParseValue());
                    SkipWhitespace();

                    if (Peek() == ',')
                    {
                        Advance();
                        continue;
                    }
                    if (Peek() == ']')
                    {
                        Advance();
                        return arr;
                    }
                    return {}; // Malformed
                }
            }

            Value ParseObject()
            {
                Advance(); // consume '{'
                SkipWhitespace();

                auto obj = Value::MakeObject();
                if (Peek() == '}')
                {
                    Advance();
                    return obj;
                }

                while (true)
                {
                    SkipWhitespace();
                    if (Peek() != '"')
                        return {};

                    Value keyVal = ParseString();
                    if (!keyVal.IsString())
                        return {};

                    SkipWhitespace();
                    if (!Expect(':'))
                        return {};

                    SkipWhitespace();
                    obj[keyVal.AsString()] = ParseValue();
                    SkipWhitespace();

                    if (Peek() == ',')
                    {
                        Advance();
                        continue;
                    }
                    if (Peek() == '}')
                    {
                        Advance();
                        return obj;
                    }
                    return {}; // Malformed
                }
            }

            bool MatchLiteral(const char* literal)
            {
                size_t len = 0;
                while (literal[len] != '\0')
                    ++len;
                if (m_pos + len > m_input.size())
                    return false;
                if (m_input.substr(m_pos, len) == literal)
                {
                    m_pos += len;
                    return true;
                }
                return false;
            }
        };

    } // namespace Detail

    // =========================================================================
    // Public API: Parse / Stringify
    // =========================================================================

#if SPARK_HAS_NLOHMANN_JSON
    // =========================================================================
    // nlohmann/json interop: convert between nlohmann::json and Spark::Json::Value
    // =========================================================================

    namespace Detail
    {
        inline Value FromNlohmann(const nlohmann::json& j)
        {
            if (j.is_null())
                return Value();
            if (j.is_boolean())
                return Value(j.get<bool>());
            if (j.is_number_integer())
                return Value(static_cast<double>(j.get<int64_t>()));
            if (j.is_number_unsigned())
                return Value(static_cast<double>(j.get<uint64_t>()));
            if (j.is_number_float())
                return Value(j.get<double>());
            if (j.is_string())
                return Value(j.get<std::string>());
            if (j.is_array())
            {
                auto arr = Value::MakeArray();
                for (size_t i = 0; i < j.size(); ++i)
                    arr.PushBack(FromNlohmann(j[i]));
                return arr;
            }
            if (j.is_object())
            {
                auto obj = Value::MakeObject();
                for (const auto& item : j.items())
                    obj[item.key] = FromNlohmann(item.value);
                return obj;
            }
            return Value();
        }

        inline nlohmann::json ToNlohmann(const Value& val)
        {
            switch (val.GetType())
            {
            case Type::Null:
                return nullptr;
            case Type::Bool:
                return val.AsBool();
            case Type::Number:
                return val.AsNumber();
            case Type::String:
                return val.AsString();
            case Type::Array:
            {
                auto arr = nlohmann::json::array();
                for (size_t i = 0; i < val.Size(); ++i)
                    arr.push_back(ToNlohmann(val[i]));
                return arr;
            }
            case Type::Object:
            {
                auto obj = nlohmann::json::object();
                for (const auto& key : val.GetKeys())
                    obj[key] = ToNlohmann(val[key]);
                return obj;
            }
            }
            return nullptr;
        }
    } // namespace Detail

    /**
     * @brief Convert a Spark::Json::Value to nlohmann::json
     */
    [[nodiscard]] inline nlohmann::json ToNlohmann(const Value& val)
    {
        return Detail::ToNlohmann(val);
    }

    /**
     * @brief Convert a nlohmann::json to Spark::Json::Value
     */
    [[nodiscard]] inline Value FromNlohmann(const nlohmann::json& j)
    {
        return Detail::FromNlohmann(j);
    }
#endif // SPARK_HAS_NLOHMANN_JSON

    /**
     * @brief Parse a JSON string into a Value. Returns Null on malformed input.
     *
     * When SPARK_HAS_NLOHMANN_JSON is defined, uses nlohmann/json for parsing
     * (more robust, handles edge cases, Unicode escapes). Otherwise falls back
     * to the built-in recursive-descent parser.
     */
    [[nodiscard]] inline Value Parse(std::string_view json)
    {
#if SPARK_HAS_NLOHMANN_JSON
        try
        {
            auto j = nlohmann::json::parse(json);
            return Detail::FromNlohmann(j);
        }
        catch (...)
        {
            // nlohmann parse failed — return null (matches existing behavior)
            return {};
        }
#else
        Detail::Parser parser(json);
        return parser.Parse();
#endif
    }

    namespace Detail
    {

        inline void EscapeString(std::string& out, const std::string& str)
        {
            out += '"';
            for (char c : str)
            {
                switch (c)
                {
                case '"':
                    out += "\\\"";
                    break;
                case '\\':
                    out += "\\\\";
                    break;
                case '\b':
                    out += "\\b";
                    break;
                case '\f':
                    out += "\\f";
                    break;
                case '\n':
                    out += "\\n";
                    break;
                case '\r':
                    out += "\\r";
                    break;
                case '\t':
                    out += "\\t";
                    break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        // Control character — use \u00XX
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                        out += buf;
                    }
                    else
                    {
                        out += c;
                    }
                    break;
                }
            }
            out += '"';
        }

        inline void StringifyImpl(std::string& out, const Value& val)
        {
            switch (val.GetType())
            {
            case Type::Null:
                out += "null";
                break;
            case Type::Bool:
                out += val.AsBool() ? "true" : "false";
                break;
            case Type::Number:
            {
                double num = val.AsNumber();
                // Use integer representation when the value is a whole number
                if (num == static_cast<double>(static_cast<int64_t>(num)) && num >= -1e15 && num <= 1e15)
                    out += std::to_string(static_cast<int64_t>(num));
                else
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.17g", num);
                    out += buf;
                }
                break;
            }
            case Type::String:
                EscapeString(out, val.AsString());
                break;
            case Type::Array:
            {
                out += '[';
                for (size_t i = 0; i < val.Size(); ++i)
                {
                    if (i > 0)
                        out += ',';
                    StringifyImpl(out, val[i]);
                }
                out += ']';
                break;
            }
            case Type::Object:
            {
                out += '{';
                auto keys = val.GetKeys();
                bool first = true;
                for (const auto& key : keys)
                {
                    if (!first)
                        out += ',';
                    first = false;
                    EscapeString(out, key);
                    out += ':';
                    StringifyImpl(out, val[key]);
                }
                out += '}';
                break;
            }
            }
        }

        inline void PrettyImpl(std::string& out, const Value& val, int indent, int depth)
        {
            std::string pad(static_cast<size_t>(depth * indent), ' ');
            std::string innerPad(static_cast<size_t>((depth + 1) * indent), ' ');

            switch (val.GetType())
            {
            case Type::Null:
                out += "null";
                break;
            case Type::Bool:
                out += val.AsBool() ? "true" : "false";
                break;
            case Type::Number:
            {
                double num = val.AsNumber();
                if (num == static_cast<double>(static_cast<int64_t>(num)) && num >= -1e15 && num <= 1e15)
                    out += std::to_string(static_cast<int64_t>(num));
                else
                {
                    char buf[64];
                    std::snprintf(buf, sizeof(buf), "%.17g", num);
                    out += buf;
                }
                break;
            }
            case Type::String:
                EscapeString(out, val.AsString());
                break;
            case Type::Array:
            {
                if (val.Size() == 0)
                {
                    out += "[]";
                    break;
                }
                out += "[\n";
                for (size_t i = 0; i < val.Size(); ++i)
                {
                    out += innerPad;
                    PrettyImpl(out, val[i], indent, depth + 1);
                    if (i + 1 < val.Size())
                        out += ',';
                    out += '\n';
                }
                out += pad;
                out += ']';
                break;
            }
            case Type::Object:
            {
                auto keys = val.GetKeys();
                if (keys.empty())
                {
                    out += "{}";
                    break;
                }
                out += "{\n";
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    out += innerPad;
                    EscapeString(out, keys[i]);
                    out += ": ";
                    PrettyImpl(out, val[keys[i]], indent, depth + 1);
                    if (i + 1 < keys.size())
                        out += ',';
                    out += '\n';
                }
                out += pad;
                out += '}';
                break;
            }
            }
        }

    } // namespace Detail

    /**
     * @brief Serialize a Value to compact JSON.
     */
    [[nodiscard]] inline std::string Stringify(const Value& val)
    {
        std::string out;
        Detail::StringifyImpl(out, val);
        return out;
    }

    /**
     * @brief Serialize a Value to pretty-printed JSON with indentation.
     * @param val    The value to serialize.
     * @param indent Number of spaces per indentation level.
     */
    [[nodiscard]] inline std::string StringifyPretty(const Value& val, int indent = 2)
    {
        std::string out;
        Detail::PrettyImpl(out, val, indent, 0);
        return out;
    }

} // namespace Spark::Json
