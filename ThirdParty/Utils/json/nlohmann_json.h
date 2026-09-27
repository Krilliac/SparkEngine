/**
 * nlohmann/json - v3.11.x - JSON for Modern C++
 * https://github.com/nlohmann/json
 *
 * This is a MINIMAL STUB providing the nlohmann::json API surface used by
 * SparkEngine. For production use, replace with the real single-header from:
 *   curl -L https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp -o ThirdParty/Utils/json/nlohmann_json.h
 *
 * License: MIT
 */

#ifndef NLOHMANN_JSON_HPP
#define NLOHMANN_JSON_HPP

#include <cstdint>
#include <initializer_list>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nlohmann
{

    /**
     * @brief Minimal JSON value type for SparkEngine integration.
     *
     * Supports: null, bool, int64, uint64, double, string, array, object.
     * This stub covers the most common nlohmann::json API surface. For
     * advanced features (SAX parser, BSON, CBOR, etc.) use the real header.
     */
    class json
    {
      public:
        using array_t = std::vector<json>;
        using object_t = std::map<std::string, json>;

        enum class value_t : uint8_t
        {
            null,
            boolean,
            number_integer,
            number_unsigned,
            number_float,
            string,
            array,
            object
        };

        // -- Construction --

        json() : m_type(value_t::null) {}
        json(std::nullptr_t) : m_type(value_t::null) {}
        json(bool b) : m_type(value_t::boolean), m_bool(b) {}
        json(int val) : m_type(value_t::number_integer), m_int(val) {}
        json(int64_t val) : m_type(value_t::number_integer), m_int(val) {}
        json(uint64_t val) : m_type(value_t::number_unsigned), m_uint(val) {}
        json(double val) : m_type(value_t::number_float), m_float(val) {}
        json(const char* str) : m_type(value_t::string), m_string(str) {}
        json(const std::string& str) : m_type(value_t::string), m_string(str) {}
        json(std::string&& str) : m_type(value_t::string), m_string(std::move(str)) {}

        json(std::initializer_list<json> init)
        {
            bool is_object = true;
            for (const auto& elem : init)
            {
                if (elem.m_type != value_t::array || elem.m_array.size() != 2 ||
                    elem.m_array[0].m_type != value_t::string)
                {
                    is_object = false;
                    break;
                }
            }

            if (is_object && init.size() > 0)
            {
                m_type = value_t::object;
                for (const auto& elem : init)
                {
                    m_object[elem.m_array[0].m_string] = elem.m_array[1];
                }
            }
            else
            {
                m_type = value_t::array;
                m_array = array_t(init);
            }
        }

        /// Deepest array/object nesting parse() accepts. Engine documents (scenes,
        /// glTF, manifests) nest well under a dozen levels.
        static constexpr size_t max_parse_depth = 256;

        // Named constructors
        static json array() { return json(value_t::array); }
        static json object() { return json(value_t::object); }

        /**
         * Parse exactly one JSON document. Like the real library, malformed
         * input throws instead of yielding a partial value: truncated arrays,
         * objects and strings, missing separators, invalid literals or escapes,
         * and any non-whitespace after the root value. A torn or truncated file
         * must never load as a valid-looking (often empty) document.
         *
         * Nesting deeper than max_parse_depth arrays/objects throws as well. The
         * parser is recursive, so an unbounded depth turns a few kilobytes of '['
         * into a native stack overflow, which no C++ catch handler can recover.
         */
        static json parse(std::string_view input)
        {
            size_t pos = 0;
            json result = parse_value(input, pos, 0);
            skip_ws(input, pos);
            if (pos != input.size())
                parse_fail("trailing content after the JSON value", pos);
            return result;
        }

        // -- Type query --

        [[nodiscard]] value_t type() const { return m_type; }
        [[nodiscard]] bool is_null() const { return m_type == value_t::null; }
        [[nodiscard]] bool is_boolean() const { return m_type == value_t::boolean; }
        [[nodiscard]] bool is_number() const
        {
            return m_type == value_t::number_integer || m_type == value_t::number_unsigned ||
                   m_type == value_t::number_float;
        }
        [[nodiscard]] bool is_number_integer() const { return m_type == value_t::number_integer; }
        [[nodiscard]] bool is_number_unsigned() const { return m_type == value_t::number_unsigned; }
        [[nodiscard]] bool is_number_float() const { return m_type == value_t::number_float; }
        [[nodiscard]] bool is_string() const { return m_type == value_t::string; }
        [[nodiscard]] bool is_array() const { return m_type == value_t::array; }
        [[nodiscard]] bool is_object() const { return m_type == value_t::object; }

        // -- Value access --

        template <typename T> T get() const;

        // String conversion
        [[nodiscard]] const std::string& get_ref() const
        {
            if (m_type != value_t::string)
                throw std::runtime_error("json: not a string");
            return m_string;
        }

        // Implicit conversion helpers
        operator bool() const
        {
            if (m_type == value_t::boolean)
                return m_bool;
            throw std::runtime_error("json: not a boolean");
        }

        // -- Object access --

        json& operator[](const char* key) { return operator[](std::string(key)); }
        const json& operator[](const char* key) const { return operator[](std::string(key)); }

        json& operator[](const std::string& key)
        {
            if (m_type == value_t::null)
                m_type = value_t::object;
            if (m_type != value_t::object)
                throw std::runtime_error("json: not an object");
            return m_object[key];
        }

        const json& operator[](const std::string& key) const
        {
            if (m_type != value_t::object)
                throw std::runtime_error("json: not an object");
            auto it = m_object.find(key);
            if (it == m_object.end())
            {
                static const json s_null;
                return s_null;
            }
            return it->second;
        }

        json& at(const std::string& key)
        {
            if (m_type != value_t::object)
                throw std::runtime_error("json: not an object");
            auto it = m_object.find(key);
            if (it == m_object.end())
                throw std::runtime_error("json: key not found: " + key);
            return it->second;
        }

        const json& at(const std::string& key) const
        {
            if (m_type != value_t::object)
                throw std::runtime_error("json: not an object");
            auto it = m_object.find(key);
            if (it == m_object.end())
                throw std::runtime_error("json: key not found: " + key);
            return it->second;
        }

        [[nodiscard]] bool contains(const char* key) const { return contains(std::string(key)); }

        [[nodiscard]] bool contains(const std::string& key) const
        {
            if (m_type != value_t::object)
                return false;
            return m_object.count(key) > 0;
        }

        // -- Array access --

        json& operator[](size_t index)
        {
            if (m_type != value_t::array)
                throw std::runtime_error("json: not an array");
            return m_array[index];
        }

        const json& operator[](size_t index) const
        {
            if (m_type != value_t::array)
                throw std::runtime_error("json: not an array");
            return m_array[index];
        }

        void push_back(const json& val)
        {
            if (m_type == value_t::null)
                m_type = value_t::array;
            if (m_type != value_t::array)
                throw std::runtime_error("json: not an array");
            m_array.push_back(val);
        }

        [[nodiscard]] size_t size() const
        {
            if (m_type == value_t::array)
                return m_array.size();
            if (m_type == value_t::object)
                return m_object.size();
            return 0;
        }

        [[nodiscard]] bool empty() const { return size() == 0; }

        // -- Iteration --

        auto begin() { return m_type == value_t::array ? m_array.begin() : m_array.begin(); }
        auto end() { return m_type == value_t::array ? m_array.end() : m_array.end(); }
        auto begin() const { return m_type == value_t::array ? m_array.begin() : m_array.begin(); }
        auto end() const { return m_type == value_t::array ? m_array.end() : m_array.end(); }

        // Object iteration via items()
        struct item_t
        {
            const std::string& key;
            const json& value;
        };

        class items_proxy
        {
          public:
            explicit items_proxy(const object_t& obj) : m_obj(obj) {}

            class iterator
            {
              public:
                explicit iterator(object_t::const_iterator it) : m_it(it) {}
                item_t operator*() const { return {m_it->first, m_it->second}; }
                iterator& operator++()
                {
                    ++m_it;
                    return *this;
                }
                bool operator!=(const iterator& other) const { return m_it != other.m_it; }

              private:
                object_t::const_iterator m_it;
            };

            iterator begin() const { return iterator(m_obj.begin()); }
            iterator end() const { return iterator(m_obj.end()); }

          private:
            const object_t& m_obj;
        };

        items_proxy items() const
        {
            static const object_t s_empty;
            return items_proxy(m_type == value_t::object ? m_object : s_empty);
        }

        // -- Serialization --

        [[nodiscard]] std::string dump(int indent = -1) const
        {
            std::string out;
            dump_impl(out, indent, 0);
            return out;
        }

        // -- Comparison --

        bool operator==(const json& other) const
        {
            if (m_type != other.m_type)
                return false;
            switch (m_type)
            {
            case value_t::null:
                return true;
            case value_t::boolean:
                return m_bool == other.m_bool;
            case value_t::number_integer:
                return m_int == other.m_int;
            case value_t::number_unsigned:
                return m_uint == other.m_uint;
            case value_t::number_float:
                return m_float == other.m_float;
            case value_t::string:
                return m_string == other.m_string;
            case value_t::array:
                return m_array == other.m_array;
            case value_t::object:
                return m_object == other.m_object;
            }
            return false;
        }

        bool operator!=(const json& other) const { return !(*this == other); }

        // Allow json to be used with value_from / value_to patterns
        template <typename T> [[nodiscard]] T value(const std::string& key, const T& default_value) const
        {
            if (m_type != value_t::object)
                return default_value;
            auto it = m_object.find(key);
            if (it == m_object.end())
                return default_value;
            return it->second.get<T>();
        }

      private:
        explicit json(value_t t) : m_type(t) {}

        value_t m_type = value_t::null;
        bool m_bool = false;
        int64_t m_int = 0;
        uint64_t m_uint = 0;
        double m_float = 0.0;
        std::string m_string;
        array_t m_array;
        object_t m_object;

        // -- Minimal JSON parser (strict: malformed input throws) --

        [[noreturn]] static void parse_fail(const char* what, size_t pos)
        {
            throw std::runtime_error(std::string("json: parse error at offset ") + std::to_string(pos) + ": " + what);
        }

        static void skip_ws(std::string_view s, size_t& pos)
        {
            while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
                ++pos;
        }

        static bool is_digit(std::string_view s, size_t pos) { return pos < s.size() && s[pos] >= '0' && s[pos] <= '9'; }

        static json parse_value(std::string_view s, size_t& pos, size_t depth)
        {
            skip_ws(s, pos);
            if (pos >= s.size())
                parse_fail("unexpected end of input", pos);

            char c = s[pos];
            if (c == '"')
                return parse_string(s, pos);
            if (c == '{' || c == '[')
            {
                // depth counts the containers already open around this value.
                if (depth >= max_parse_depth)
                    parse_fail("nesting too deep", pos);
                return c == '{' ? parse_object(s, pos, depth + 1) : parse_array(s, pos, depth + 1);
            }
            if (c == 't' || c == 'f')
                return parse_bool(s, pos);
            if (c == 'n')
                return parse_null(s, pos);
            if (c == '-' || (c >= '0' && c <= '9'))
                return parse_number(s, pos);
            parse_fail("unexpected character", pos);
        }

        static json parse_null(std::string_view s, size_t& pos)
        {
            if (s.substr(pos, 4) == "null")
            {
                pos += 4;
                return {};
            }
            parse_fail("invalid literal", pos);
        }

        static json parse_bool(std::string_view s, size_t& pos)
        {
            if (s.substr(pos, 4) == "true")
            {
                pos += 4;
                return json(true);
            }
            if (s.substr(pos, 5) == "false")
            {
                pos += 5;
                return json(false);
            }
            parse_fail("invalid literal", pos);
        }

        static json parse_number(std::string_view s, size_t& pos)
        {
            const size_t start = pos;
            bool has_dot = false;
            bool has_exp = false;
            bool negative = false;

            // JSON grammar: -?digits(.digits)?([eE][+-]?digits)? -- every part
            // that is started must have at least one digit.
            if (s[pos] == '-')
            {
                negative = true;
                ++pos;
            }
            if (!is_digit(s, pos))
                parse_fail("expected digit", pos);
            while (is_digit(s, pos))
                ++pos;

            if (pos < s.size() && s[pos] == '.')
            {
                has_dot = true;
                ++pos;
                if (!is_digit(s, pos))
                    parse_fail("expected digit after '.'", pos);
                while (is_digit(s, pos))
                    ++pos;
            }

            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E'))
            {
                has_exp = true;
                ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-'))
                    ++pos;
                if (!is_digit(s, pos))
                    parse_fail("expected digit in exponent", pos);
                while (is_digit(s, pos))
                    ++pos;
            }

            std::string numStr(s.substr(start, pos - start));
            if (has_dot || has_exp)
            {
                return json(std::stod(numStr));
            }
            else if (negative)
            {
                return json(static_cast<int64_t>(std::stoll(numStr)));
            }
            else
            {
                int64_t val = std::stoll(numStr);
                if (val >= 0)
                    return json(static_cast<int64_t>(val));
                return json(val);
            }
        }

        static uint32_t parse_hex4(std::string_view s, size_t& pos)
        {
            if (pos + 4 > s.size())
                parse_fail("truncated \\u escape", pos);
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
            {
                const char h = s[pos++];
                value <<= 4;
                if (h >= '0' && h <= '9')
                    value |= static_cast<uint32_t>(h - '0');
                else if (h >= 'a' && h <= 'f')
                    value |= static_cast<uint32_t>(h - 'a' + 10);
                else if (h >= 'A' && h <= 'F')
                    value |= static_cast<uint32_t>(h - 'A' + 10);
                else
                    parse_fail("invalid \\u escape", pos - 1);
            }
            return value;
        }

        // Decode the code point after "\u" (joining a UTF-16 surrogate pair) and append it as UTF-8.
        static void append_unicode_escape(std::string_view s, size_t& pos, std::string& out)
        {
            uint32_t cp = parse_hex4(s, pos);
            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                if (s.substr(pos, 2) != "\\u")
                    parse_fail("unpaired high surrogate", pos);
                pos += 2;
                const uint32_t low = parse_hex4(s, pos);
                if (low < 0xDC00 || low > 0xDFFF)
                    parse_fail("invalid low surrogate", pos - 4);
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
            {
                parse_fail("unpaired low surrogate", pos - 4);
            }

            if (cp < 0x80)
            {
                out += static_cast<char>(cp);
            }
            else if (cp < 0x800)
            {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else if (cp < 0x10000)
            {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
            else
            {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        static json parse_string(std::string_view s, size_t& pos)
        {
            if (pos >= s.size() || s[pos] != '"')
                parse_fail("expected string", pos);
            ++pos;

            std::string result;
            while (pos < s.size())
            {
                char c = s[pos++];
                if (c == '"')
                    return json(std::move(result));
                if (c == '\\')
                {
                    if (pos >= s.size())
                        break;
                    char esc = s[pos++];
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
                        append_unicode_escape(s, pos, result);
                        break;
                    default:
                        parse_fail("invalid escape", pos - 1);
                    }
                }
                else if (static_cast<unsigned char>(c) < 0x20)
                {
                    parse_fail("unescaped control character in string", pos - 1);
                }
                else
                {
                    result += c;
                }
            }
            parse_fail("unterminated string", pos);
        }

        static json parse_array(std::string_view s, size_t& pos, size_t depth)
        {
            ++pos; // consume '['
            skip_ws(s, pos);

            json arr(value_t::array);
            if (pos < s.size() && s[pos] == ']')
            {
                ++pos;
                return arr;
            }

            // A truncated array runs out of input inside parse_value or here;
            // either way it throws rather than returning the elements so far.
            while (true)
            {
                arr.m_array.push_back(parse_value(s, pos, depth));
                skip_ws(s, pos);
                if (pos >= s.size())
                    parse_fail("unterminated array", pos);
                if (s[pos] == ',')
                {
                    ++pos;
                    continue;
                }
                if (s[pos] == ']')
                {
                    ++pos;
                    return arr;
                }
                parse_fail("expected ',' or ']'", pos);
            }
        }

        static json parse_object(std::string_view s, size_t& pos, size_t depth)
        {
            ++pos; // consume '{'
            skip_ws(s, pos);

            json obj(value_t::object);
            if (pos < s.size() && s[pos] == '}')
            {
                ++pos;
                return obj;
            }

            while (true)
            {
                skip_ws(s, pos);
                json key = parse_string(s, pos);

                skip_ws(s, pos);
                if (pos >= s.size() || s[pos] != ':')
                    parse_fail("expected ':'", pos);
                ++pos;

                obj.m_object[key.m_string] = parse_value(s, pos, depth);
                skip_ws(s, pos);

                if (pos >= s.size())
                    parse_fail("unterminated object", pos);
                if (s[pos] == ',')
                {
                    ++pos;
                    continue;
                }
                if (s[pos] == '}')
                {
                    ++pos;
                    return obj;
                }
                parse_fail("expected ',' or '}'", pos);
            }
        }

        // -- Serialization --

        static void escape_string(std::string& out, const std::string& str)
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
                    // The parser rejects raw control characters, so dump must escape them.
                    if (static_cast<unsigned char>(c) < 0x20)
                    {
                        static constexpr char kHex[] = "0123456789abcdef";
                        out += "\\u00";
                        out += kHex[(c >> 4) & 0xF];
                        out += kHex[c & 0xF];
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

        void dump_impl(std::string& out, int indent, int depth) const
        {
            std::string pad;
            std::string inner_pad;
            bool pretty = indent >= 0;
            if (pretty)
            {
                pad = std::string(depth * indent, ' ');
                inner_pad = std::string((depth + 1) * indent, ' ');
            }

            switch (m_type)
            {
            case value_t::null:
                out += "null";
                break;
            case value_t::boolean:
                out += m_bool ? "true" : "false";
                break;
            case value_t::number_integer:
                out += std::to_string(m_int);
                break;
            case value_t::number_unsigned:
                out += std::to_string(m_uint);
                break;
            case value_t::number_float:
            {
                std::ostringstream ss;
                ss << m_float;
                std::string s = ss.str();
                // Ensure decimal point
                if (s.find('.') == std::string::npos && s.find('e') == std::string::npos)
                    s += ".0";
                out += s;
                break;
            }
            case value_t::string:
                escape_string(out, m_string);
                break;
            case value_t::array:
            {
                if (m_array.empty())
                {
                    out += "[]";
                    break;
                }
                out += pretty ? "[\n" : "[";
                for (size_t i = 0; i < m_array.size(); ++i)
                {
                    if (pretty)
                        out += inner_pad;
                    m_array[i].dump_impl(out, indent, depth + 1);
                    if (i + 1 < m_array.size())
                        out += ",";
                    if (pretty)
                        out += "\n";
                }
                if (pretty)
                    out += pad;
                out += "]";
                break;
            }
            case value_t::object:
            {
                if (m_object.empty())
                {
                    out += "{}";
                    break;
                }
                out += pretty ? "{\n" : "{";
                size_t idx = 0;
                for (const auto& [k, v] : m_object)
                {
                    if (pretty)
                        out += inner_pad;
                    escape_string(out, k);
                    out += pretty ? ": " : ":";
                    v.dump_impl(out, indent, depth + 1);
                    if (idx + 1 < m_object.size())
                        out += ",";
                    if (pretty)
                        out += "\n";
                    ++idx;
                }
                if (pretty)
                    out += pad;
                out += "}";
                break;
            }
            }
        }
    };

    // -- Template specializations for get<T>() --

    template <> inline std::string json::get<std::string>() const
    {
        if (m_type == value_t::string)
            return m_string;
        throw std::runtime_error("json: not a string");
    }

    template <> inline bool json::get<bool>() const
    {
        if (m_type == value_t::boolean)
            return m_bool;
        throw std::runtime_error("json: not a boolean");
    }

    template <> inline int json::get<int>() const
    {
        if (m_type == value_t::number_integer)
            return static_cast<int>(m_int);
        if (m_type == value_t::number_unsigned)
            return static_cast<int>(m_uint);
        if (m_type == value_t::number_float)
            return static_cast<int>(m_float);
        throw std::runtime_error("json: not a number");
    }

    template <> inline int64_t json::get<int64_t>() const
    {
        if (m_type == value_t::number_integer)
            return m_int;
        if (m_type == value_t::number_unsigned)
            return static_cast<int64_t>(m_uint);
        if (m_type == value_t::number_float)
            return static_cast<int64_t>(m_float);
        throw std::runtime_error("json: not a number");
    }

    template <> inline uint64_t json::get<uint64_t>() const
    {
        if (m_type == value_t::number_unsigned)
            return m_uint;
        if (m_type == value_t::number_integer)
            return static_cast<uint64_t>(m_int);
        throw std::runtime_error("json: not a number");
    }

    template <> inline double json::get<double>() const
    {
        if (m_type == value_t::number_float)
            return m_float;
        if (m_type == value_t::number_integer)
            return static_cast<double>(m_int);
        if (m_type == value_t::number_unsigned)
            return static_cast<double>(m_uint);
        throw std::runtime_error("json: not a number");
    }

    template <> inline float json::get<float>() const
    {
        return static_cast<float>(get<double>());
    }

    // String literal operator for JSON parsing
    inline json operator""_json(const char* s, size_t n)
    {
        return json::parse(std::string_view(s, n));
    }

} // namespace nlohmann

#endif // NLOHMANN_JSON_HPP
