/**
 * @file ProjectDocument.cpp
 * @brief .sparkproject and RecentProjects.json field scrape (moved out of ProjectManager.cpp)
 */

#include "ProjectDocument.h"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

namespace SparkEditor
{

    namespace
    {
        int JsonHexValue(char c)
        {
            if (c >= '0' && c <= '9')
            {
                return c - '0';
            }
            if (c >= 'a' && c <= 'f')
            {
                return c - 'a' + 10;
            }
            if (c >= 'A' && c <= 'F')
            {
                return c - 'A' + 10;
            }
            return -1;
        }

        bool ParseJsonHex4(const std::string& json, size_t offset, uint32_t& value)
        {
            if (offset + 4 > json.size())
            {
                return false;
            }
            value = 0;
            for (size_t index = 0; index < 4; ++index)
            {
                const int digit = JsonHexValue(json[offset + index]);
                if (digit < 0)
                {
                    return false;
                }
                value = (value << 4) | static_cast<uint32_t>(digit);
            }
            return true;
        }

        bool AppendUtf8(uint32_t codePoint, std::string& output)
        {
            if (codePoint > 0x10FFFF || (codePoint >= 0xD800 && codePoint <= 0xDFFF))
            {
                return false;
            }
            if (codePoint <= 0x7F)
            {
                output.push_back(static_cast<char>(codePoint));
            }
            else if (codePoint <= 0x7FF)
            {
                output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else if (codePoint <= 0xFFFF)
            {
                output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            else
            {
                output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
                output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
            }
            return true;
        }

        // `index` points at the character immediately after a JSON backslash and
        // advances across any consumed unicode digits/surrogate pair.
        bool DecodeJsonEscape(const std::string& json, size_t& index, std::string& output)
        {
            switch (json[index])
            {
            case '"':
                output += '"';
                return true;
            case '\\':
                output += '\\';
                return true;
            case '/':
                output += '/';
                return true;
            case 'b':
                output += '\b';
                return true;
            case 'f':
                output += '\f';
                return true;
            case 'n':
                output += '\n';
                return true;
            case 'r':
                output += '\r';
                return true;
            case 't':
                output += '\t';
                return true;
            case 'u':
            {
                uint32_t first = 0;
                if (!ParseJsonHex4(json, index + 1, first))
                {
                    return false;
                }
                index += 4;
                if (first >= 0xD800 && first <= 0xDBFF)
                {
                    if (index + 6 >= json.size() || json[index + 1] != '\\' || json[index + 2] != 'u')
                    {
                        return false;
                    }
                    uint32_t second = 0;
                    if (!ParseJsonHex4(json, index + 3, second) || second < 0xDC00 || second > 0xDFFF)
                    {
                        return false;
                    }
                    first = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
                    index += 6;
                }
                else if (first >= 0xDC00 && first <= 0xDFFF)
                {
                    return false;
                }
                return AppendUtf8(first, output);
            }
            default:
                return false;
            }
        }

        /// Offset just past the ':' of the first `"key"` followed (after optional whitespace) by
        /// ':', or npos. An occurrence followed by anything else is a string value that spells
        /// the key (a module named "template", say), not the key itself.
        size_t FindKeyValue(const std::string& json, const std::string& key)
        {
            const std::string quoted = "\"" + key + "\"";
            for (size_t found = json.find(quoted); found != std::string::npos; found = json.find(quoted, found + 1))
            {
                size_t after = found + quoted.size();
                while (after < json.size() &&
                       (json[after] == ' ' || json[after] == '\t' || json[after] == '\r' || json[after] == '\n'))
                {
                    ++after;
                }
                if (after < json.size() && json[after] == ':')
                {
                    return after + 1;
                }
            }
            return std::string::npos;
        }

        std::string ExtractJsonString(const std::string& json, const std::string& key)
        {
            size_t pos = FindKeyValue(json, key);
            if (pos == std::string::npos)
            {
                return "";
            }
            pos = json.find('\"', pos);
            if (pos == std::string::npos)
            {
                return "";
            }
            std::string result;
            for (size_t i = pos + 1; i < json.size(); ++i)
            {
                const char c = json[i];
                if (c == '\"')
                {
                    return result;
                }
                if (c != '\\')
                {
                    result += c;
                    continue;
                }
                if (++i >= json.size())
                {
                    return "";
                }
                if (!DecodeJsonEscape(json, i, result))
                {
                    return "";
                }
            }
            return "";
        }

        /// An absent or non-numeric field reads as 0. A number that does not fit 64 bits returns
        /// false: std::stoull used to throw std::out_of_range out of the whole document read.
        bool ExtractJsonUint64(const std::string& json, const std::string& key, uint64_t& value)
        {
            value = 0;
            size_t pos = FindKeyValue(json, key);
            if (pos == std::string::npos)
            {
                return true;
            }
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t'))
            {
                pos++;
            }
            const size_t start = pos;
            while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9')
            {
                pos++;
            }
            if (start == pos)
            {
                return true;
            }
            const auto [next, error] = std::from_chars(json.data() + start, json.data() + pos, value);
            if (error != std::errc{} || next != json.data() + pos)
            {
                value = 0;
                return false;
            }
            return true;
        }

        /// The array's string elements, scanned string-aware: a ']' inside an element (a scene
        /// named "Level[2]") no longer ends the array and drops every element after it.
        std::vector<std::string> ExtractJsonStringArray(const std::string& json, const std::string& key)
        {
            std::vector<std::string> result;
            size_t pos = FindKeyValue(json, key);
            if (pos == std::string::npos)
            {
                return result;
            }
            pos = json.find('[', pos);
            if (pos == std::string::npos)
            {
                return result;
            }

            size_t i = pos + 1;
            while (i < json.size())
            {
                if (json[i] == ']')
                {
                    return result;
                }
                if (json[i] != '\"')
                {
                    ++i;
                    continue;
                }
                std::string value;
                bool closed = false;
                for (++i; i < json.size(); ++i)
                {
                    const char c = json[i];
                    if (c == '\"')
                    {
                        ++i;
                        closed = true;
                        break;
                    }
                    if (c == '\\')
                    {
                        ++i;
                        if (i >= json.size() || !DecodeJsonEscape(json, i, value))
                        {
                            return {};
                        }
                        continue;
                    }
                    value += c;
                }
                if (!closed)
                {
                    return {};
                }
                result.push_back(std::move(value));
            }
            // No closing bracket: the document is truncated inside the array.
            return {};
        }

        /// Next occurrence of @p target at or after @p from that is not inside a string literal,
        /// or npos. @p from must itself be outside every string literal.
        size_t FindOutsideString(const std::string& text, char target, size_t from)
        {
            bool inString = false;
            for (size_t i = from; i < text.size(); ++i)
            {
                const char c = text[i];
                if (inString)
                {
                    if (c == '\\')
                    {
                        ++i;
                    }
                    else if (c == '\"')
                    {
                        inString = false;
                    }
                    continue;
                }
                if (c == target)
                {
                    return i;
                }
                if (c == '\"')
                {
                    inString = true;
                }
            }
            return std::string::npos;
        }

        void WriteStringArray(std::ostringstream& out, const std::vector<std::string>& values)
        {
            for (size_t i = 0; i < values.size(); ++i)
            {
                out << "    \"" << EscapeProjectJsonString(values[i]) << "\"";
                if (i + 1 < values.size())
                {
                    out << ",";
                }
                out << "\n";
            }
        }
    } // namespace

    std::string EscapeProjectJsonString(const std::string& s)
    {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char c : s)
        {
            switch (c)
            {
            case '\"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
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
            case '\b':
                out += "\\b";
                break;
            case '\f':
                out += "\\f";
                break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                {
                    constexpr char hex[] = "0123456789ABCDEF";
                    const auto value = static_cast<unsigned char>(c);
                    out += "\\u00";
                    out += hex[value >> 4];
                    out += hex[value & 0x0F];
                }
                else
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    ProjectDocumentStatus CheckProjectDocument(const std::string& content, std::string& reason)
    {
        constexpr std::string_view kWhitespace = " \t\r\n";
        const size_t first = content.find_first_not_of(kWhitespace);
        if (first == std::string::npos)
        {
            reason = "the file is empty";
            return ProjectDocumentStatus::Rejected;
        }
        const size_t last = content.find_last_not_of(kWhitespace);
        if (content[first] != '{' || content[last] != '}')
        {
            reason = "the file is not a complete JSON object (it is truncated or damaged)";
            return ProjectDocumentStatus::Rejected;
        }

        const std::string supported = std::to_string(ProjectManager::kProjectFileVersion);
        constexpr std::string_view kVersionKey = "\"projectFileVersion\"";
        const size_t keyPos = content.find(kVersionKey);
        if (keyPos == std::string::npos)
        {
            return ProjectDocumentStatus::Ok;
        }

        size_t valuePos = content.find_first_not_of(kWhitespace, keyPos + kVersionKey.size());
        if (valuePos != std::string::npos && content[valuePos] == ':')
        {
            valuePos = content.find_first_not_of(kWhitespace, valuePos + 1);
        }
        else
        {
            valuePos = std::string::npos;
        }
        const char* const end = content.data() + content.size();
        const char* const begin = valuePos == std::string::npos ? end : content.data() + valuePos;
        uint64_t version = 0;
        const auto [next, parseError] = std::from_chars(begin, end, version);
        if (parseError != std::errc{} ||
            (next != end && kWhitespace.find(*next) == std::string_view::npos && *next != ',' && *next != '}'))
        {
            reason = "\"projectFileVersion\" is not an unsigned integer";
            return ProjectDocumentStatus::Rejected;
        }
        if (version > ProjectManager::kProjectFileVersion)
        {
            reason = "the file declares projectFileVersion " + std::to_string(version) + "; this build reads " +
                     supported + " and writes " + supported + ". Open it with a newer SparkEditor";
            return ProjectDocumentStatus::NewerVersion;
        }
        if (version == 0)
        {
            reason = "the file declares projectFileVersion 0, which no SparkEditor has written; this build reads " +
                     supported + " and writes " + supported;
            return ProjectDocumentStatus::Rejected;
        }
        return ProjectDocumentStatus::Ok;
    }

    bool ReadProjectDocumentFields(const std::string& content, ProjectDocumentFields& fields, std::string& reason)
    {
        fields = ProjectDocumentFields{};
        fields.name = ExtractJsonString(content, "name");
        fields.version = ExtractJsonString(content, "version");
        fields.description = ExtractJsonString(content, "description");
        fields.engineVersion = ExtractJsonString(content, "engineVersion");
        fields.templateId = ExtractJsonString(content, "template");
        fields.defaultScene = ExtractJsonString(content, "defaultScene");
        fields.lastOpenedScene = ExtractJsonString(content, "lastOpenedScene");
        fields.scenes = ExtractJsonStringArray(content, "scenes");
        fields.modules = ExtractJsonStringArray(content, "modules");
        const auto readTimestamp = [&](const char* key, uint64_t& value)
        {
            if (ExtractJsonUint64(content, key, value))
            {
                return true;
            }
            reason = std::string("\"") + key + "\" is not a 64-bit unsigned integer";
            return false;
        };
        if (!readTimestamp("lastModified", fields.lastModified) || !readTimestamp("createdTime", fields.createdTime))
        {
            fields = ProjectDocumentFields{};
            return false;
        }
        return true;
    }

    std::string WriteProjectDocument(const ProjectDocumentFields& fields)
    {
        std::ostringstream file;
        file << "{\n";
        file << "  \"projectFileVersion\": " << ProjectManager::kProjectFileVersion << ",\n";
        file << R"(  "name": ")" << EscapeProjectJsonString(fields.name) << "\",\n";
        file << R"(  "version": ")" << EscapeProjectJsonString(fields.version) << "\",\n";
        file << R"(  "description": ")" << EscapeProjectJsonString(fields.description) << "\",\n";
        file << R"(  "engineVersion": ")" << EscapeProjectJsonString(fields.engineVersion) << "\",\n";
        if (!fields.templateId.empty())
        {
            file << R"(  "template": ")" << EscapeProjectJsonString(fields.templateId) << "\",\n";
        }
        file << R"(  "defaultScene": ")" << EscapeProjectJsonString(fields.defaultScene) << "\",\n";
        file << R"(  "lastOpenedScene": ")" << EscapeProjectJsonString(fields.lastOpenedScene) << "\",\n";
        file << "  \"createdTime\": " << fields.createdTime << ",\n";
        file << "  \"lastModified\": " << fields.lastModified << ",\n";

        file << "  \"modules\": [\n";
        WriteStringArray(file, fields.modules);
        file << "  ],\n";

        file << "  \"scenes\": [\n";
        WriteStringArray(file, fields.scenes);
        file << "  ]\n";
        file << "}\n";
        return file.str();
    }

    std::vector<RecentProject> ReadRecentProjectsDocument(const std::string& content)
    {
        // Format: { "recentProjects": [ { "name": ..., "path": ..., ... }, ... ] }
        // Braces inside string values are skipped, so a project named "Game}" keeps its entry.
        std::vector<RecentProject> projects;
        const bool hasOuterObject = content.find("\"recentProjects\"") != std::string::npos;
        size_t pos = 0;
        while (true)
        {
            pos = FindOutsideString(content, '{', pos);
            if (pos == std::string::npos)
            {
                break;
            }

            // Skip the outer object
            if (hasOuterObject && pos == 0)
            {
                pos++;
                continue;
            }

            const size_t end = FindOutsideString(content, '}', pos);
            if (end == std::string::npos)
            {
                break;
            }

            const std::string entry = content.substr(pos, end - pos + 1);
            RecentProject project;
            project.path = ExtractJsonString(entry, "path");
            if (!project.path.empty())
            {
                project.name = ExtractJsonString(entry, "name");
                project.engineVersion = ExtractJsonString(entry, "engineVersion");
                if (ExtractJsonUint64(entry, "lastOpened", project.lastOpened))
                {
                    projects.push_back(std::move(project));
                }
            }
            pos = end + 1;
        }
        return projects;
    }

    std::string WriteRecentProjectsDocument(std::span<const RecentProject> projects)
    {
        std::ostringstream file;
        file << "{\n";
        file << "  \"recentProjects\": [\n";
        const size_t count = std::min(projects.size(), kMaxRecentProjects);
        for (size_t i = 0; i < count; ++i)
        {
            const auto& rp = projects[i];
            file << "    {\n";
            file << R"(      "name": ")" << EscapeProjectJsonString(rp.name) << "\",\n";
            file << R"(      "path": ")" << EscapeProjectJsonString(rp.path) << "\",\n";
            file << R"(      "engineVersion": ")" << EscapeProjectJsonString(rp.engineVersion) << "\",\n";
            file << "      \"lastOpened\": " << rp.lastOpened << "\n";
            file << "    }";
            if (i + 1 < count)
            {
                file << ",";
            }
            file << "\n";
        }
        file << "  ]\n";
        file << "}\n";
        return file.str();
    }

} // namespace SparkEditor
