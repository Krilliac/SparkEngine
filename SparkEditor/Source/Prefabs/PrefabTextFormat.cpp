/**
 * @file PrefabTextFormat.cpp
 * @brief The `.sparkprefab` text grammar: parse versions N-1..N, render version N
 *
 * Version 1 wrote names and string values as bare text, so it could not store a component or
 * property name containing whitespace or a string value spanning lines, and a file cut inside
 * its last value still parsed. Version 2 writes every name and string value as a double-quoted
 * token with `\\`, `\"`, `\n`, `\r` and `\t` escapes, writes numbers in their shortest
 * round-trip form (std::to_chars), and closes the file with an `end` line.
 */

#include "PrefabTextFormat.h"
#include "Utils/StringUtils.h"

#include <algorithm>
#include <charconv>
#include <initializer_list>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>

namespace SparkEditor::PrefabTextFormat
{
    namespace
    {
        // Far above anything the editor authors. A larger count means a damaged file, and
        // bounding it keeps a corrupt count from driving a huge parse loop.
        constexpr long long kMaxPrefabComponents = 4096;
        constexpr long long kMaxPrefabProperties = 4096;
        constexpr const char* kKnownPropertyTypes = "bool, int, float, double, string, float3, float4";

        std::string SupportedWindow()
        {
            return "this build reads versions " + std::to_string(PrefabAsset::kOldestSupportedPrefabVersion) + " to " +
                   std::to_string(PrefabAsset::kPrefabFormatVersion);
        }

        /// Line reader that tracks 1-based line numbers and accepts CRLF checkouts.
        class PrefabLineReader
        {
          public:
            explicit PrefabLineReader(const std::string& text) : m_input(text) {}

            bool Next(std::string& line)
            {
                if (!std::getline(m_input, line))
                {
                    return false;
                }
                ++m_lineNumber;
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                return true;
            }

            int LineNumber() const { return m_lineNumber; }

          private:
            std::istringstream m_input;
            int m_lineNumber = 0;
        };

        /// `<keyword> <rest>`; false when @p line does not start with the keyword and one space.
        bool SplitKeyword(const std::string& line, std::string_view keyword, std::string& rest)
        {
            if (line.size() <= keyword.size() || !line.starts_with(keyword) || line[keyword.size()] != ' ')
            {
                return false;
            }
            rest = line.substr(keyword.size() + 1);
            return true;
        }

        bool ParseCount(const std::string& text, long long maximum, long long& count)
        {
            const char* first = text.data();
            const char* last = text.data() + text.size();
            const auto [end, ec] = std::from_chars(first, last, count);
            return ec == std::errc{} && end == last && count >= 0 && count <= maximum;
        }

        /// Version 1 values: reads every value from @p text and requires nothing but whitespace after them.
        template <typename... T> bool ParseValues(const std::string& text, T&... values)
        {
            std::istringstream stream(text);
            stream.imbue(std::locale::classic());
            (stream >> ... >> values);
            if (stream.fail())
            {
                return false;
            }
            stream >> std::ws;
            return stream.eof();
        }

        /// Version 2 line tokens: quoted names and strings, bare words and numbers, one space apart.
        class TokenCursor
        {
          public:
            explicit TokenCursor(std::string_view text) : m_text(text) {}

            bool AtEnd() const { return m_pos == m_text.size(); }

            bool Literal(std::string_view literal)
            {
                if (!m_text.substr(m_pos).starts_with(literal))
                {
                    return false;
                }
                m_pos += literal.size();
                return true;
            }

            bool Word(std::string_view& word)
            {
                size_t end = m_text.find(' ', m_pos);
                if (end == std::string_view::npos)
                {
                    end = m_text.size();
                }
                if (end == m_pos)
                {
                    return false;
                }
                word = m_text.substr(m_pos, end - m_pos);
                m_pos = end;
                return true;
            }

            /// A space, then a number that from_chars (or, for floats, its portable twin) consumes completely.
            template <typename T> bool SpacedNumber(T& value)
            {
                std::string_view word;
                if (!Literal(" ") || !Word(word))
                {
                    return false;
                }
                if constexpr (std::is_floating_point_v<T>)
                {
                    const std::optional<T> parsed = Spark::StringUtils::ParseFloatingExact<T>(word);
                    if (parsed)
                    {
                        value = *parsed;
                    }
                    return parsed.has_value();
                }
                else
                {
                    const auto [end, ec] = std::from_chars(word.data(), word.data() + word.size(), value);
                    return ec == std::errc{} && end == word.data() + word.size();
                }
            }

            bool Quoted(std::string& out)
            {
                if (!Literal("\""))
                {
                    return false;
                }
                out.clear();
                while (m_pos < m_text.size())
                {
                    const char c = m_text[m_pos++];
                    if (c == '"')
                    {
                        return true;
                    }
                    if (c != '\\')
                    {
                        out += c;
                        continue;
                    }
                    if (m_pos == m_text.size())
                    {
                        return false;
                    }
                    switch (m_text[m_pos++])
                    {
                    case '\\':
                        out += '\\';
                        break;
                    case '"':
                        out += '"';
                        break;
                    case 'n':
                        out += '\n';
                        break;
                    case 'r':
                        out += '\r';
                        break;
                    case 't':
                        out += '\t';
                        break;
                    default:
                        return false;
                    }
                }
                return false; // unterminated
            }

          private:
            std::string_view m_text;
            size_t m_pos = 0;
        };

        /// Version 2 `<keyword> "<text>"` line with nothing after the closing quote.
        bool ParseQuotedLine(const std::string& line, std::string_view keyword, std::string& out)
        {
            std::string rest;
            if (!SplitKeyword(line, keyword, rest))
            {
                return false;
            }
            TokenCursor cursor(rest);
            return cursor.Quoted(out) && cursor.AtEnd() && !out.empty();
        }

        void AppendQuoted(std::string& out, std::string_view text)
        {
            out += '"';
            for (const char c : text)
            {
                switch (c)
                {
                case '\\':
                    out += "\\\\";
                    break;
                case '"':
                    out += "\\\"";
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
                    out += c;
                    break;
                }
            }
            out += '"';
        }

        /// Shortest text that from_chars reads back to exactly @p value.
        template <typename T> void AppendNumber(std::string& out, T value)
        {
            char buffer[64];
            const auto [end, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
            if (ec == std::errc{})
            {
                out.append(buffer, end);
            }
        }

        bool AddProperty(SerializedComponent& component, const std::string& propName, PrefabPropertyValue value,
                         const std::string& location, std::string& reason)
        {
            if (component.properties.emplace(propName, std::move(value)).second)
            {
                return true;
            }
            reason = location + "component '" + component.typeName + "' declares property '" + propName + "' twice";
            return false;
        }

        std::string UnknownTypeReason(const std::string& location, const std::string& propName,
                                      const std::string& componentType, std::string_view propType)
        {
            return location + "property '" + propName + "' of component '" + componentType + "' has unknown type '" +
                   std::string(propType) + "' (known: " + kKnownPropertyTypes + ")";
        }

        bool ParsePropertyV1(const std::string& line, int lineNumber, SerializedComponent& component,
                             std::string& reason)
        {
            const std::string location = "line " + std::to_string(lineNumber) + ": ";
            std::istringstream fields(line);
            std::string propName;
            std::string propType;
            if (!(fields >> propName >> propType))
            {
                reason =
                    location + "component '" + component.typeName + "' has a property line without a name and type";
                return false;
            }
            std::string value;
            std::getline(fields, value);
            if (!value.empty() && value.front() == ' ')
            {
                value.erase(0, 1);
            }

            const auto malformed = [&]()
            {
                reason = location + "property '" + propName + "' of component '" + component.typeName +
                         "' has a malformed " + propType + " value '" + value + "'";
                return false;
            };

            PrefabPropertyValue parsed;
            if (propType == "bool")
            {
                if (value != "true" && value != "false")
                {
                    return malformed();
                }
                parsed = (value == "true");
            }
            else if (propType == "int")
            {
                int v = 0;
                if (!ParseValues(value, v))
                {
                    return malformed();
                }
                parsed = v;
            }
            else if (propType == "float")
            {
                float v = 0.0f;
                if (!ParseValues(value, v))
                {
                    return malformed();
                }
                parsed = v;
            }
            else if (propType == "double")
            {
                double v = 0.0;
                if (!ParseValues(value, v))
                {
                    return malformed();
                }
                parsed = v;
            }
            else if (propType == "string")
            {
                parsed = value;
            }
            else if (propType == "float3")
            {
                XMFLOAT3 v{0.0f, 0.0f, 0.0f};
                if (!ParseValues(value, v.x, v.y, v.z))
                {
                    return malformed();
                }
                parsed = v;
            }
            else if (propType == "float4")
            {
                XMFLOAT4 v{0.0f, 0.0f, 0.0f, 0.0f};
                if (!ParseValues(value, v.x, v.y, v.z, v.w))
                {
                    return malformed();
                }
                parsed = v;
            }
            else
            {
                reason = UnknownTypeReason(location, propName, component.typeName, propType);
                return false;
            }
            return AddProperty(component, propName, std::move(parsed), location, reason);
        }

        /// `  "<name>" <type> <value...>`, where a string value is quoted and numbers are one space apart.
        bool ParsePropertyV2(const std::string& line, int lineNumber, SerializedComponent& component,
                             std::string& reason)
        {
            const std::string location = "line " + std::to_string(lineNumber) + ": ";
            TokenCursor cursor(line);
            std::string propName;
            std::string_view propType;
            if (!cursor.Literal("  ") || !cursor.Quoted(propName) || propName.empty() || !cursor.Literal(" ") ||
                !cursor.Word(propType))
            {
                reason = location + "component '" + component.typeName +
                         "' has a property line without a quoted name and a type";
                return false;
            }

            PrefabPropertyValue parsed;
            bool wellFormed = false;
            if (propType == "bool")
            {
                std::string_view word;
                wellFormed = cursor.Literal(" ") && cursor.Word(word) && (word == "true" || word == "false");
                parsed = (word == "true");
            }
            else if (propType == "int")
            {
                int v = 0;
                wellFormed = cursor.SpacedNumber(v);
                parsed = v;
            }
            else if (propType == "float")
            {
                float v = 0.0f;
                wellFormed = cursor.SpacedNumber(v);
                parsed = v;
            }
            else if (propType == "double")
            {
                double v = 0.0;
                wellFormed = cursor.SpacedNumber(v);
                parsed = v;
            }
            else if (propType == "string")
            {
                std::string v;
                wellFormed = cursor.Literal(" ") && cursor.Quoted(v);
                parsed = std::move(v);
            }
            else if (propType == "float3")
            {
                XMFLOAT3 v{0.0f, 0.0f, 0.0f};
                wellFormed = cursor.SpacedNumber(v.x) && cursor.SpacedNumber(v.y) && cursor.SpacedNumber(v.z);
                parsed = v;
            }
            else if (propType == "float4")
            {
                XMFLOAT4 v{0.0f, 0.0f, 0.0f, 0.0f};
                wellFormed = cursor.SpacedNumber(v.x) && cursor.SpacedNumber(v.y) && cursor.SpacedNumber(v.z) &&
                             cursor.SpacedNumber(v.w);
                parsed = v;
            }
            else
            {
                reason = UnknownTypeReason(location, propName, component.typeName, propType);
                return false;
            }

            if (!wellFormed || !cursor.AtEnd())
            {
                reason = location + "property '" + propName + "' of component '" + component.typeName +
                         "' has a malformed " + std::string(propType) + " value";
                return false;
            }
            return AddProperty(component, propName, std::move(parsed), location, reason);
        }

        /// `component <type>` (version 1, one bare word) or `component "<type>"` (version 2).
        bool ParseComponentLine(const std::string& line, bool quoted, std::string& typeName)
        {
            if (quoted)
            {
                return ParseQuotedLine(line, "component", typeName);
            }
            return SplitKeyword(line, "component", typeName) && !typeName.empty() &&
                   typeName.find_first_of(" \t") == std::string::npos;
        }
    } // namespace

    ParseResult Parse(const std::string& text, ParsedPrefab& out, std::string& reason)
    {
        PrefabLineReader reader(text);
        std::string line;
        std::string rest;

        if (!reader.Next(line))
        {
            reason = "the file is empty";
            return ParseResult::Rejected;
        }
        long long version = 0;
        if (!SplitKeyword(line, "SPARKPREFAB", rest) || !ParseCount(rest, std::numeric_limits<int>::max(), version))
        {
            reason = "line 1 is not a 'SPARKPREFAB <version>' header";
            return ParseResult::Rejected;
        }
        if (version > PrefabAsset::kPrefabFormatVersion)
        {
            reason = "the file is format version " + std::to_string(version) + "; " + SupportedWindow() +
                     ". Open it with a newer SparkEditor";
            return ParseResult::NewerVersion;
        }
        if (version < PrefabAsset::kOldestSupportedPrefabVersion)
        {
            reason = "line 1 declares format version " + std::to_string(version) +
                     ", which no SparkEditor has written; " + SupportedWindow();
            return ParseResult::Rejected;
        }
        // Version 1 names and strings are bare text; version 2 quotes and escapes them.
        const bool quoted = version >= 2;
        const auto parseProperty = quoted ? ParsePropertyV2 : ParsePropertyV1;

        const bool nameOk = reader.Next(line) && (quoted ? ParseQuotedLine(line, "name", out.name)
                                                         : SplitKeyword(line, "name", out.name) && !out.name.empty());
        if (!nameOk)
        {
            reason = quoted ? "line 2 must be 'name \"<prefab name>\"'" : "line 2 must be 'name <prefab name>'";
            return ParseResult::Rejected;
        }

        long long componentCount = 0;
        if (!reader.Next(line) || !SplitKeyword(line, "components", rest) ||
            !ParseCount(rest, kMaxPrefabComponents, componentCount))
        {
            reason =
                "line 3 must be 'components <count>' with a count from 0 to " + std::to_string(kMaxPrefabComponents);
            return ParseResult::Rejected;
        }

        out.components.clear();
        for (long long i = 0; i < componentCount; ++i)
        {
            const std::string position = "component " + std::to_string(i + 1) + " of " + std::to_string(componentCount);
            SerializedComponent component;
            if (!reader.Next(line))
            {
                reason = "the file is truncated at " + position;
                return ParseResult::Rejected;
            }
            if (!ParseComponentLine(line, quoted, component.typeName))
            {
                reason = "line " + std::to_string(reader.LineNumber()) + " must be " +
                         (quoted ? "'component \"<type>\"'" : "'component <type>'") + " for " + position;
                return ParseResult::Rejected;
            }
            const std::string named = position + " ('" + component.typeName + "')";

            long long propertyCount = 0;
            if (!reader.Next(line))
            {
                reason = "the file is truncated at " + named;
                return ParseResult::Rejected;
            }
            if (!SplitKeyword(line, "properties", rest) || !ParseCount(rest, kMaxPrefabProperties, propertyCount))
            {
                reason = "line " + std::to_string(reader.LineNumber()) +
                         " must be 'properties <count>' with a count from 0 to " +
                         std::to_string(kMaxPrefabProperties) + " for " + named;
                return ParseResult::Rejected;
            }

            for (long long j = 0; j < propertyCount; ++j)
            {
                if (!reader.Next(line))
                {
                    reason = "the file is truncated at property " + std::to_string(j + 1) + " of " +
                             std::to_string(propertyCount) + " in " + named;
                    return ParseResult::Rejected;
                }
                if (!parseProperty(line, reader.LineNumber(), component, reason))
                {
                    return ParseResult::Rejected;
                }
            }
            out.components.push_back(std::move(component));
        }

        // Version 2 closes with `end`, so a cut inside the last value cannot pass as a shorter value.
        if (quoted)
        {
            if (!reader.Next(line))
            {
                reason = "the file is truncated: the closing 'end' line after the declared " +
                         std::to_string(componentCount) + " components is missing";
                return ParseResult::Rejected;
            }
            if (line != "end")
            {
                reason = "line " + std::to_string(reader.LineNumber()) + " must be 'end' after the declared " +
                         std::to_string(componentCount) + " components";
                return ParseResult::Rejected;
            }
        }

        while (reader.Next(line))
        {
            if (line.find_first_not_of(" \t") != std::string::npos)
            {
                reason = "line " + std::to_string(reader.LineNumber()) + " has content after the declared " +
                         std::to_string(componentCount) + " components";
                return ParseResult::Rejected;
            }
        }
        return ParseResult::Ok;
    }

    std::string Render(const std::string& name, const std::vector<SerializedComponent>& components)
    {
        std::string text = "SPARKPREFAB " + std::to_string(PrefabAsset::kPrefabFormatVersion) + "\nname ";
        AppendQuoted(text, name);
        text += "\ncomponents " + std::to_string(components.size()) + "\n";

        for (const auto& comp : components)
        {
            text += "component ";
            AppendQuoted(text, comp.typeName);
            text += "\nproperties " + std::to_string(comp.properties.size()) + "\n";

            std::vector<const std::pair<const std::string, PrefabPropertyValue>*> sorted;
            sorted.reserve(comp.properties.size());
            for (const auto& property : comp.properties)
            {
                sorted.push_back(&property);
            }
            std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->first < b->first; });

            for (const auto* property : sorted)
            {
                text += "  ";
                AppendQuoted(text, property->first);
                std::visit(
                    [&text](const auto& val)
                    {
                        using T = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<T, bool>)
                        {
                            text += val ? " bool true" : " bool false";
                        }
                        else if constexpr (std::is_same_v<T, int>)
                        {
                            text += " int ";
                            AppendNumber(text, val);
                        }
                        else if constexpr (std::is_same_v<T, float>)
                        {
                            text += " float ";
                            AppendNumber(text, val);
                        }
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            text += " double ";
                            AppendNumber(text, val);
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                        {
                            text += " string ";
                            AppendQuoted(text, val);
                        }
                        else if constexpr (std::is_same_v<T, XMFLOAT3>)
                        {
                            text += " float3";
                            for (const float component : {val.x, val.y, val.z})
                            {
                                text += ' ';
                                AppendNumber(text, component);
                            }
                        }
                        else if constexpr (std::is_same_v<T, XMFLOAT4>)
                        {
                            text += " float4";
                            for (const float component : {val.x, val.y, val.z, val.w})
                            {
                                text += ' ';
                                AppendNumber(text, component);
                            }
                        }
                    },
                    property->second);
                text += '\n';
            }
        }
        text += "end\n";
        return text;
    }

} // namespace SparkEditor::PrefabTextFormat
