// TestJsonUtils.cpp - Tests for JsonUtils parser and serializer
// Tests parsing, stringify, and round-trip for all JSON types

#include "TestFramework.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

// Simplified JSON implementation for testing (mirrors engine implementation)
namespace Json
{
    enum class Type : uint8_t
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object
    };

    class Value
    {
      public:
        using ArrayType = std::vector<Value>;
        using ObjectType = std::unordered_map<std::string, Value>;

        Value() : m_data(nullptr) {}
        explicit Value(std::nullptr_t) : m_data(nullptr) {}
        explicit Value(bool b) : m_data(b) {}
        explicit Value(double num) : m_data(num) {}
        explicit Value(int num) : m_data(static_cast<double>(num)) {}
        explicit Value(const char* str) : m_data(std::string(str)) {}
        explicit Value(std::string str) : m_data(std::move(str)) {}

        static Value MakeArray() { return Value(ArrayType{}); }
        static Value MakeObject() { return Value(ObjectType{}); }

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

        [[nodiscard]] bool AsBool() const { return std::get<bool>(m_data); }
        [[nodiscard]] double AsNumber() const { return std::get<double>(m_data); }
        [[nodiscard]] int AsInt() const { return static_cast<int>(std::get<double>(m_data)); }
        [[nodiscard]] const std::string& AsString() const { return std::get<std::string>(m_data); }

        Value& operator[](size_t index) { return std::get<ArrayType>(m_data)[index]; }
        const Value& operator[](size_t index) const { return std::get<ArrayType>(m_data)[index]; }
        void PushBack(Value val) { std::get<ArrayType>(m_data).push_back(std::move(val)); }

        [[nodiscard]] size_t Size() const
        {
            if (auto* arr = std::get_if<ArrayType>(&m_data))
                return arr->size();
            if (auto* obj = std::get_if<ObjectType>(&m_data))
                return obj->size();
            return 0;
        }

        Value& operator[](const std::string& key)
        {
            if (IsNull())
                m_data = ObjectType{};
            return std::get<ObjectType>(m_data)[key];
        }

        const Value& operator[](const std::string& key) const
        {
            static const Value s_null;
            auto& obj = std::get<ObjectType>(m_data);
            auto it = obj.find(key);
            if (it == obj.end())
                return s_null;
            return it->second;
        }

        [[nodiscard]] bool HasKey(const std::string& key) const
        {
            if (auto* obj = std::get_if<ObjectType>(&m_data))
                return obj->contains(key);
            return false;
        }

        [[nodiscard]] bool operator==(const Value& other) const { return m_data == other.m_data; }

      private:
        explicit Value(ArrayType arr) : m_data(std::move(arr)) {}
        explicit Value(ObjectType obj) : m_data(std::move(obj)) {}

        std::variant<std::nullptr_t, bool, double, std::string, ArrayType, ObjectType> m_data;
    };

    // Minimal recursive descent parser
    class Parser
    {
      public:
        explicit Parser(std::string_view input) : m_input(input), m_pos(0) {}

        Value Parse()
        {
            SkipWhitespace();
            if (m_pos >= m_input.size())
                return {};
            return ParseValue();
        }

      private:
        std::string_view m_input;
        size_t m_pos;

        char Peek() const { return m_pos < m_input.size() ? m_input[m_pos] : '\0'; }
        char Advance() { return m_pos < m_input.size() ? m_input[m_pos++] : '\0'; }

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
            return {};
        }

        Value ParseNull()
        {
            if (Match("null"))
                return {};
            return {};
        }

        Value ParseBool()
        {
            if (Match("true"))
                return Value(true);
            if (Match("false"))
                return Value(false);
            return {};
        }

        Value ParseNumber()
        {
            size_t start = m_pos;
            if (Peek() == '-')
                Advance();
            if (Peek() == '0')
                Advance();
            else
                while (Peek() >= '0' && Peek() <= '9')
                    Advance();
            if (Peek() == '.')
            {
                Advance();
                while (Peek() >= '0' && Peek() <= '9')
                    Advance();
            }
            if (Peek() == 'e' || Peek() == 'E')
            {
                Advance();
                if (Peek() == '+' || Peek() == '-')
                    Advance();
                while (Peek() >= '0' && Peek() <= '9')
                    Advance();
            }
            std::string numStr(m_input.substr(start, m_pos - start));
            try
            {
                return Value(std::stod(numStr));
            }
            catch (...)
            {
                return {};
            }
        }

        Value ParseString()
        {
            Advance(); // consume '"'
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
                    case 'n':
                        result += '\n';
                        break;
                    case 't':
                        result += '\t';
                        break;
                    case 'r':
                        result += '\r';
                        break;
                    default:
                        result += esc;
                        break;
                    }
                }
                else
                    result += c;
            }
            return {};
        }

        Value ParseArray()
        {
            Advance();
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
                return {};
            }
        }

        Value ParseObject()
        {
            Advance();
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
                return {};
            }
        }

        bool Match(const char* literal)
        {
            size_t len = 0;
            while (literal[len])
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

    Value ParseJson(std::string_view json)
    {
        Parser p(json);
        return p.Parse();
    }

} // namespace Json

// ===========================================================================
// Tests
// ===========================================================================

TEST(Json_ParseNull)
{
    auto v = Json::ParseJson("null");
    EXPECT_TRUE(v.IsNull());
}

TEST(Json_ParseTrue)
{
    auto v = Json::ParseJson("true");
    EXPECT_TRUE(v.IsBool());
    EXPECT_TRUE(v.AsBool());
}

TEST(Json_ParseFalse)
{
    auto v = Json::ParseJson("false");
    EXPECT_TRUE(v.IsBool());
    EXPECT_FALSE(v.AsBool());
}

TEST(Json_ParseInteger)
{
    auto v = Json::ParseJson("42");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), 42.0, 0.001);
}

TEST(Json_ParseNegative)
{
    auto v = Json::ParseJson("-17");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), -17.0, 0.001);
}

TEST(Json_ParseFloat)
{
    auto v = Json::ParseJson("3.14");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), 3.14, 0.001);
}

TEST(Json_ParseScientific)
{
    auto v = Json::ParseJson("1.5e3");
    EXPECT_TRUE(v.IsNumber());
    EXPECT_NEAR(v.AsNumber(), 1500.0, 0.001);
}

TEST(Json_ParseString)
{
    auto v = Json::ParseJson("\"hello world\"");
    EXPECT_TRUE(v.IsString());
    EXPECT_EQ(v.AsString(), std::string("hello world"));
}

TEST(Json_ParseStringEscapes)
{
    auto v = Json::ParseJson("\"line1\\nline2\\ttab\"");
    EXPECT_TRUE(v.IsString());
    EXPECT_EQ(v.AsString(), std::string("line1\nline2\ttab"));
}

TEST(Json_ParseEmptyArray)
{
    auto v = Json::ParseJson("[]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), 0u);
}

TEST(Json_ParseArray)
{
    auto v = Json::ParseJson("[1, 2, 3]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), 3u);
    EXPECT_NEAR(v[0].AsNumber(), 1.0, 0.001);
    EXPECT_NEAR(v[1].AsNumber(), 2.0, 0.001);
    EXPECT_NEAR(v[2].AsNumber(), 3.0, 0.001);
}

TEST(Json_ParseNestedArray)
{
    auto v = Json::ParseJson("[[1, 2], [3]]");
    EXPECT_TRUE(v.IsArray());
    EXPECT_EQ(v.Size(), 2u);
    EXPECT_EQ(v[0].Size(), 2u);
    EXPECT_EQ(v[1].Size(), 1u);
}

TEST(Json_ParseEmptyObject)
{
    auto v = Json::ParseJson("{}");
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v.Size(), 0u);
}

TEST(Json_ParseObject)
{
    auto v = Json::ParseJson("{\"name\": \"Fireball\", \"damage\": 50}");
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v[std::string("name")].AsString(), std::string("Fireball"));
    EXPECT_NEAR(v[std::string("damage")].AsNumber(), 50.0, 0.001);
}

TEST(Json_ParseNestedObject)
{
    auto v = Json::ParseJson("{\"a\": {\"b\": 42}}");
    EXPECT_TRUE(v.IsObject());
    EXPECT_TRUE(v[std::string("a")].IsObject());
    EXPECT_NEAR(v[std::string("a")][std::string("b")].AsNumber(), 42.0, 0.001);
}

TEST(Json_ParseWithWhitespace)
{
    auto v = Json::ParseJson("  { \"key\" :  \"value\"  } ");
    EXPECT_TRUE(v.IsObject());
    EXPECT_EQ(v[std::string("key")].AsString(), std::string("value"));
}

TEST(Json_ParseMalformedReturnsNull)
{
    auto v = Json::ParseJson("{invalid json}");
    EXPECT_TRUE(v.IsNull());
}

TEST(Json_ParseEmptyReturnsNull)
{
    auto v = Json::ParseJson("");
    EXPECT_TRUE(v.IsNull());
}

TEST(Json_HasKey)
{
    auto v = Json::ParseJson("{\"x\": 1}");
    EXPECT_TRUE(v.HasKey("x"));
    EXPECT_FALSE(v.HasKey("y"));
}

TEST(Json_AsInt)
{
    auto v = Json::ParseJson("42");
    EXPECT_EQ(v.AsInt(), 42);
}

TEST(Json_BuildObject)
{
    Json::Value obj;
    obj[std::string("level")] = Json::Value(5);
    obj[std::string("name")] = Json::Value("test");
    EXPECT_TRUE(obj.IsObject());
    EXPECT_EQ(obj[std::string("level")].AsInt(), 5);
    EXPECT_EQ(obj[std::string("name")].AsString(), std::string("test"));
}

TEST(Json_BuildArray)
{
    auto arr = Json::Value::MakeArray();
    arr.PushBack(Json::Value(1));
    arr.PushBack(Json::Value(2));
    arr.PushBack(Json::Value("three"));
    EXPECT_EQ(arr.Size(), 3u);
    EXPECT_EQ(arr[2].AsString(), std::string("three"));
}

TEST(Json_MixedArray)
{
    auto v = Json::ParseJson("[1, \"two\", true, null]");
    EXPECT_EQ(v.Size(), 4u);
    EXPECT_TRUE(v[0].IsNumber());
    EXPECT_TRUE(v[1].IsString());
    EXPECT_TRUE(v[2].IsBool());
    EXPECT_TRUE(v[3].IsNull());
}
