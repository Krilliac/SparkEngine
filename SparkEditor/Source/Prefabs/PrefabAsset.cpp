/**
 * @file PrefabAsset.cpp
 * @brief Implementation of the PrefabAsset class
 * @author Spark Engine Team
 * @date 2025
 */

#include "PrefabAsset.h"
#include "Engine/SaveSystem/SaveFileDurability.h"
#include "Utils/LogMacros.h"
#include "Utils/Validate.h"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace SparkEditor
{
    namespace
    {
        // Far above anything the editor authors. A larger count means a damaged file, and
        // bounding it keeps a corrupt count from driving a huge parse loop.
        constexpr long long kMaxPrefabComponents = 4096;
        constexpr long long kMaxPrefabProperties = 4096;
        constexpr const char* kKnownPropertyTypes = "bool, int, float, double, string, float3, float4";

        std::filesystem::path PathFromUtf8(const std::string& path)
        {
            return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
        }

        bool HasWhitespace(const std::string& text)
        {
            return text.find_first_of(" \t\r\n") != std::string::npos;
        }

        bool SpansLines(const std::string& text)
        {
            return text.find_first_of("\r\n") != std::string::npos;
        }

        /// Line reader that tracks 1-based line numbers and accepts CRLF checkouts.
        class PrefabLineReader
        {
          public:
            explicit PrefabLineReader(const std::string& text) : m_input(text) {}

            bool Next(std::string& line)
            {
                if (!std::getline(m_input, line))
                    return false;
                ++m_lineNumber;
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
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
                return false;
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

        /// Reads every value from @p text and requires nothing but whitespace after them.
        template <typename... T> bool ParseValues(const std::string& text, T&... values)
        {
            std::istringstream stream(text);
            stream.imbue(std::locale::classic());
            (stream >> ... >> values);
            if (stream.fail())
                return false;
            stream >> std::ws;
            return stream.eof();
        }

        struct ParsedPrefab
        {
            std::string name;
            std::vector<SerializedComponent> components;
        };

        enum class PrefabParseResult
        {
            Ok,
            Rejected,
            NewerVersion
        };

        bool ParseProperty(const std::string& line, const std::string& componentType, int lineNumber,
                           SerializedComponent& component, std::string& reason)
        {
            const std::string location = "line " + std::to_string(lineNumber) + ": ";
            std::istringstream fields(line);
            std::string propName;
            std::string propType;
            if (!(fields >> propName >> propType))
            {
                reason = location + "component '" + componentType + "' has a property line without a name and type";
                return false;
            }
            std::string value;
            std::getline(fields, value);
            if (!value.empty() && value.front() == ' ')
                value.erase(0, 1);

            const auto malformed = [&]()
            {
                reason = location + "property '" + propName + "' of component '" + componentType +
                         "' has a malformed " + propType + " value '" + value + "'";
                return false;
            };

            PrefabPropertyValue parsed;
            if (propType == "bool")
            {
                if (value != "true" && value != "false")
                    return malformed();
                parsed = (value == "true");
            }
            else if (propType == "int")
            {
                int v = 0;
                if (!ParseValues(value, v))
                    return malformed();
                parsed = v;
            }
            else if (propType == "float")
            {
                float v = 0.0f;
                if (!ParseValues(value, v))
                    return malformed();
                parsed = v;
            }
            else if (propType == "double")
            {
                double v = 0.0;
                if (!ParseValues(value, v))
                    return malformed();
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
                    return malformed();
                parsed = v;
            }
            else if (propType == "float4")
            {
                XMFLOAT4 v{0.0f, 0.0f, 0.0f, 0.0f};
                if (!ParseValues(value, v.x, v.y, v.z, v.w))
                    return malformed();
                parsed = v;
            }
            else
            {
                reason = location + "property '" + propName + "' of component '" + componentType +
                         "' has unknown type '" + propType + "' (known: " + kKnownPropertyTypes + ")";
                return false;
            }

            if (!component.properties.emplace(propName, std::move(parsed)).second)
            {
                reason = location + "component '" + componentType + "' declares property '" + propName + "' twice";
                return false;
            }
            return true;
        }

        PrefabParseResult ParsePrefabText(const std::string& text, ParsedPrefab& out, std::string& reason)
        {
            PrefabLineReader reader(text);
            std::string line;
            std::string rest;

            if (!reader.Next(line))
            {
                reason = "the file is empty";
                return PrefabParseResult::Rejected;
            }
            long long version = 0;
            if (!SplitKeyword(line, "SPARKPREFAB", rest) || !ParseCount(rest, std::numeric_limits<int>::max(), version))
            {
                reason = "line 1 is not a 'SPARKPREFAB <version>' header";
                return PrefabParseResult::Rejected;
            }
            if (version > PrefabAsset::kPrefabFormatVersion)
            {
                reason = "the file is format version " + std::to_string(version) + "; this build reads version " +
                         std::to_string(PrefabAsset::kPrefabFormatVersion) + " only. Open it with a newer SparkEditor";
                return PrefabParseResult::NewerVersion;
            }
            if (version < 1)
            {
                reason = "line 1 declares format version " + std::to_string(version) +
                         ", which no SparkEditor has written; this build reads version " +
                         std::to_string(PrefabAsset::kPrefabFormatVersion) + " only";
                return PrefabParseResult::Rejected;
            }

            if (!reader.Next(line) || !SplitKeyword(line, "name", out.name) || out.name.empty())
            {
                reason = "line 2 must be 'name <prefab name>'";
                return PrefabParseResult::Rejected;
            }

            long long componentCount = 0;
            if (!reader.Next(line) || !SplitKeyword(line, "components", rest) ||
                !ParseCount(rest, kMaxPrefabComponents, componentCount))
            {
                reason = "line 3 must be 'components <count>' with a count from 0 to " +
                         std::to_string(kMaxPrefabComponents);
                return PrefabParseResult::Rejected;
            }

            out.components.clear();
            for (long long i = 0; i < componentCount; ++i)
            {
                const std::string position =
                    "component " + std::to_string(i + 1) + " of " + std::to_string(componentCount);
                SerializedComponent component;
                if (!reader.Next(line))
                {
                    reason = "the file is truncated at " + position;
                    return PrefabParseResult::Rejected;
                }
                if (!SplitKeyword(line, "component", component.typeName) || component.typeName.empty() ||
                    HasWhitespace(component.typeName))
                {
                    reason =
                        "line " + std::to_string(reader.LineNumber()) + " must be 'component <type>' for " + position;
                    return PrefabParseResult::Rejected;
                }
                const std::string named = position + " ('" + component.typeName + "')";

                long long propertyCount = 0;
                if (!reader.Next(line))
                {
                    reason = "the file is truncated at " + named;
                    return PrefabParseResult::Rejected;
                }
                if (!SplitKeyword(line, "properties", rest) || !ParseCount(rest, kMaxPrefabProperties, propertyCount))
                {
                    reason = "line " + std::to_string(reader.LineNumber()) +
                             " must be 'properties <count>' with a count from 0 to " +
                             std::to_string(kMaxPrefabProperties) + " for " + named;
                    return PrefabParseResult::Rejected;
                }

                for (long long j = 0; j < propertyCount; ++j)
                {
                    if (!reader.Next(line))
                    {
                        reason = "the file is truncated at property " + std::to_string(j + 1) + " of " +
                                 std::to_string(propertyCount) + " in " + named;
                        return PrefabParseResult::Rejected;
                    }
                    if (!ParseProperty(line, component.typeName, reader.LineNumber(), component, reason))
                        return PrefabParseResult::Rejected;
                }
                out.components.push_back(std::move(component));
            }

            while (reader.Next(line))
            {
                if (line.find_first_not_of(" \t") != std::string::npos)
                {
                    reason = "line " + std::to_string(reader.LineNumber()) + " has content after the declared " +
                             std::to_string(componentCount) + " components";
                    return PrefabParseResult::Rejected;
                }
            }
            return PrefabParseResult::Ok;
        }

        PrefabParseResult ReadPrefabFile(const std::filesystem::path& path, ParsedPrefab& out, std::string& reason)
        {
            std::error_code existsError;
            if (!std::filesystem::is_regular_file(path, existsError))
            {
                reason = "the file does not exist";
                return PrefabParseResult::Rejected;
            }
            std::ifstream input(path, std::ios::binary);
            if (!input.is_open())
            {
                reason = "the file could not be opened";
                return PrefabParseResult::Rejected;
            }
            const std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            if (input.bad())
            {
                reason = "the file could not be read";
                return PrefabParseResult::Rejected;
            }
            return ParsePrefabText(text, out, reason);
        }
    } // namespace

    uint64_t PrefabAsset::s_nextId = 1;

    PrefabAsset::PrefabAsset(const std::string& name) : m_name(name), m_id(s_nextId++) {}

    void PrefabAsset::AddComponent(const SerializedComponent& component)
    {
        SPARK_VALIDATE_NOT_EMPTY(Spark::LogCategory::Editor, component.typeName);
        // Replace if component of same type already exists
        auto it = std::find_if(m_components.begin(), m_components.end(),
                               [&](const SerializedComponent& c) { return c.typeName == component.typeName; });

        if (it != m_components.end())
        {
            *it = component;
        }
        else
        {
            m_components.push_back(component);
        }
        m_isModified = true;
    }

    bool PrefabAsset::RemoveComponent(const std::string& typeName)
    {
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !typeName.empty(), false);
        auto it = std::find_if(m_components.begin(), m_components.end(),
                               [&](const SerializedComponent& c) { return c.typeName == typeName; });

        if (it != m_components.end())
        {
            m_components.erase(it);
            m_isModified = true;
            return true;
        }
        return false;
    }

    bool PrefabAsset::HasComponent(const std::string& typeName) const
    {
        return std::any_of(m_components.begin(), m_components.end(),
                           [&](const SerializedComponent& c) { return c.typeName == typeName; });
    }

    const SerializedComponent* PrefabAsset::GetComponent(const std::string& typeName) const
    {
        auto it = std::find_if(m_components.begin(), m_components.end(),
                               [&](const SerializedComponent& c) { return c.typeName == typeName; });

        return (it != m_components.end()) ? &(*it) : nullptr;
    }

    bool PrefabAsset::Save(const std::string& path)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !path.empty(), false);

        // Refuse what TryLoad would reject, so a save never replaces a loadable prefab with one
        // that cannot be read back.
        if (m_name.empty() || SpansLines(m_name))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Refusing to save prefab to '%s': the prefab name is empty or spans lines", path.c_str());
            return false;
        }
        for (const auto& comp : m_components)
        {
            if (comp.typeName.empty() || HasWhitespace(comp.typeName))
            {
                SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                                "Refusing to save prefab '%s': component type '%s' is empty or contains whitespace",
                                m_name.c_str(), comp.typeName.c_str());
                return false;
            }
            for (const auto& [name, value] : comp.properties)
            {
                const auto* text = std::get_if<std::string>(&value);
                if (name.empty() || HasWhitespace(name) || (text && SpansLines(*text)))
                {
                    SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                                    "Refusing to save prefab '%s': property '%s' of component '%s' has an empty or "
                                    "whitespace name, or a string value that spans lines",
                                    m_name.c_str(), name.c_str(), comp.typeName.c_str());
                    return false;
                }
            }
        }

        std::ostringstream file;
        file.imbue(std::locale::classic());
        // Round-trip precision: a reload restores exactly the values that were saved.
        file << std::setprecision(std::numeric_limits<double>::max_digits10);
        file << "SPARKPREFAB " << kPrefabFormatVersion << "\n";
        file << "name " << m_name << "\n";
        file << "components " << m_components.size() << "\n";

        for (const auto& comp : m_components)
        {
            file << "component " << comp.typeName << "\n";
            file << "properties " << comp.properties.size() << "\n";
            for (const auto& [name, value] : comp.properties)
            {
                file << "  " << name << " ";
                std::visit(
                    [&file](auto&& val)
                    {
                        using T = std::decay_t<decltype(val)>;
                        if constexpr (std::is_same_v<T, bool>)
                        {
                            file << "bool " << (val ? "true" : "false");
                        }
                        else if constexpr (std::is_same_v<T, int>)
                        {
                            file << "int " << val;
                        }
                        else if constexpr (std::is_same_v<T, float>)
                        {
                            file << "float " << val;
                        }
                        else if constexpr (std::is_same_v<T, double>)
                        {
                            file << "double " << val;
                        }
                        else if constexpr (std::is_same_v<T, std::string>)
                        {
                            file << "string " << val;
                        }
                        else if constexpr (std::is_same_v<T, XMFLOAT3>)
                        {
                            file << "float3 " << val.x << " " << val.y << " " << val.z;
                        }
                        else if constexpr (std::is_same_v<T, XMFLOAT4>)
                        {
                            file << "float4 " << val.x << " " << val.y << " " << val.z << " " << val.w;
                        }
                    },
                    value);
                file << "\n";
            }
        }

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "Saving prefab '%s' with %zu components to: %s", m_name.c_str(),
                       m_components.size(), path.c_str());
        std::error_code writeError;
        if (!Spark::SaveFileDurability::WriteFileAtomically(PathFromUtf8(path), file.str(), true, writeError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor,
                            "Failed to save prefab '%s' to '%s': %s. The previous file and its .bak are unchanged",
                            m_name.c_str(), path.c_str(), writeError.message().c_str());
            return false;
        }

        m_filePath = path;
        m_isModified = false;
        return true;
    }

    bool PrefabAsset::TryLoad(const std::string& path, PrefabAsset& out, std::string& error)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        error.clear();
        const std::filesystem::path primary = PathFromUtf8(path);

        ParsedPrefab parsed;
        std::string primaryReason;
        const PrefabParseResult primaryResult = ReadPrefabFile(primary, parsed, primaryReason);
        if (primaryResult == PrefabParseResult::NewerVersion)
        {
            error = "Prefab '" + path + "': " + primaryReason + ".";
            return false;
        }

        if (primaryResult != PrefabParseResult::Ok)
        {
            const std::string backupName = path + ".bak";
            std::string backupReason;
            parsed = ParsedPrefab{};
            if (ReadPrefabFile(Spark::SaveFileDurability::BackupPathFor(primary), parsed, backupReason) !=
                PrefabParseResult::Ok)
            {
                error = "Prefab '" + path + "' was rejected: " + primaryReason + ". Previous-good backup '" +
                        backupName + "' was not usable: " + backupReason + ".";
                return false;
            }
            error = "Prefab '" + path + "' was rejected: " + primaryReason + ". Loaded the previous-good backup '" +
                    backupName + "' instead.";
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "%s", error.c_str());
        }

        PrefabAsset loaded;
        loaded.m_name = std::move(parsed.name);
        loaded.m_components = std::move(parsed.components);
        // A recovered prefab keeps the primary path, so the next save repairs the primary.
        loaded.m_filePath = path;
        loaded.m_isModified = false;
        out = std::move(loaded);
        return true;
    }

} // namespace SparkEditor
