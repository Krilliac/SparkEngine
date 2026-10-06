/**
 * @file StringTable.cpp
 * @brief StringTable storage and its bounded, transactional localization file loader.
 *
 * Kept apart from LocalizationSystem.cpp so the SEC-120 fuzz target links the shipped
 * loader without the language registry and its fault-isolation dependencies.
 * Thread affinity: a StringTable is not internally synchronized; LocalizationSystem
 * loads into a local table and publishes it under its mutex.
 */

#include "LocalizationSystem.h"
#include "../../Utils/LogMacros.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace Spark
{
    // =============================================================================
    // StringTable
    // =============================================================================

    namespace
    {
        /// Finds the unescaped closing quote of the JSON string whose opening quote is at
        /// `open`. Escaped characters (\" \\ \n ...) stay part of the string.
        bool FindClosingQuote(const std::string& text, size_t open, size_t& closeOut)
        {
            for (size_t i = open + 1; i < text.size(); ++i)
            {
                if (text[i] == '\\')
                {
                    ++i; // skip the escaped character
                    continue;
                }
                if (text[i] == '"')
                {
                    closeOut = i;
                    return true;
                }
            }
            return false;
        }

        size_t SkipJsonSpace(const std::string& text, size_t pos)
        {
            while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' ||
                                         text[pos] == '\r' || text[pos] == '\f' || text[pos] == '\v'))
            {
                ++pos;
            }
            return pos;
        }
    } // namespace

    bool StringTable::LoadFromFile(const std::string& filePath)
    {
        // Refuse an oversized file before reading it, and bound the read itself so a
        // file that grows after the size check still cannot be pulled in whole.
        std::error_code sizeError;
        const auto fileSize = std::filesystem::file_size(filePath, sizeError);
        if (sizeError)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "StringTable: cannot stat localization file '%s' (%s)",
                            filePath.c_str(), sizeError.message().c_str());
            return false;
        }
        if (fileSize > kMaxFileBytes)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core,
                            "StringTable: localization file '%s' is %llu bytes, above the %zu byte limit",
                            filePath.c_str(), static_cast<unsigned long long>(fileSize), kMaxFileBytes);
            return false;
        }

        std::ifstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "StringTable: failed to open localization file: %s",
                            filePath.c_str());
            return false;
        }

        std::string content(static_cast<size_t>(fileSize) + 1, '\0');
        file.read(content.data(), static_cast<std::streamsize>(content.size()));
        content.resize(static_cast<size_t>(file.gcount()));
        if (content.size() > fileSize)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "StringTable: localization file '%s' grew while being read",
                            filePath.c_str());
            return false;
        }

        // Flat JSON object of string pairs: { "key": "value", "key2": "value2" }.
        // A single linear scan replaces the former std::regex, whose backtracking
        // matcher recursed once per character: one long value overflowed the stack
        // (libstdc++) or threw an uncaught regex_error (MSVC) during module init.
        // Escaped characters (\" \\ \n) stay inside a string, so "He said \"hi\""
        // is captured whole.

        // Translate JSON backslash escapes in a captured string to their literal
        // characters. Unknown escapes keep the escaped character verbatim.
        auto unescape = [](const std::string& in)
        {
            std::string out;
            out.reserve(in.size());
            for (size_t i = 0; i < in.size(); ++i)
            {
                if (in[i] == '\\' && i + 1 < in.size())
                {
                    switch (const char next = in[++i])
                    {
                    case 'n':
                        out.push_back('\n');
                        break;
                    case 't':
                        out.push_back('\t');
                        break;
                    case 'r':
                        out.push_back('\r');
                        break;
                    default:
                        out.push_back(next);
                        break;
                    }
                }
                else
                {
                    out.push_back(in[i]);
                }
            }
            return out;
        };

        // Parse into a temporary table.  A malformed reload must not publish a
        // half-loaded catalog to readers that still hold the previous table.
        std::unordered_map<std::string, std::string> parsedEntries;
        size_t parsed = 0;
        size_t pos = 0;
        // Tolerate a UTF-8 byte order mark, which Windows editors commonly write.
        const size_t bomBytes = content.starts_with("\xEF\xBB\xBF") ? 3 : 0;
        const size_t firstContent = SkipJsonSpace(content, bomBytes);
        const size_t lastContent = content.empty() ? 0 : content.find_last_not_of(" \t\n\r\f\v");
        if (firstContent >= content.size() || content[firstContent] != '{' || lastContent == std::string::npos ||
            content[lastContent] != '}')
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core, "StringTable: localization file '%s' is not a JSON object",
                           filePath.c_str());
            return false;
        }
        while (true)
        {
            const size_t keyOpen = content.find('"', pos);
            if (keyOpen == std::string::npos)
            {
                break;
            }
            size_t keyClose = 0;
            if (!FindClosingQuote(content, keyOpen, keyClose))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core, "StringTable: unterminated key/value string in '%s'",
                               filePath.c_str());
                return false;
            }

            // A string followed by ':' and another string is one entry. Any other string
            // (a nested object's name, a non-string value's key) is skipped, and scanning
            // resumes after it, so every byte is visited a bounded number of times.
            size_t next = SkipJsonSpace(content, keyClose + 1);
            if (next < content.size() && content[next] == ':')
            {
                next = SkipJsonSpace(content, next + 1);
                size_t valueClose = 0;
                if (next < content.size() && content[next] == '"')
                {
                    if (!FindClosingQuote(content, next, valueClose))
                    {
                        SPARK_LOG_WARN(Spark::LogCategory::Core, "StringTable: unterminated value string in '%s'",
                                       filePath.c_str());
                        return false;
                    }
                    parsedEntries[unescape(content.substr(keyOpen + 1, keyClose - keyOpen - 1))] =
                        unescape(content.substr(next + 1, valueClose - next - 1));
                    ++parsed;
                    pos = valueClose + 1;
                    continue;
                }
            }
            pos = keyClose + 1;
        }

        if (parsed == 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Core,
                           "StringTable: parsed 0 entries from '%s' (%zu content bytes) — bad JSON format?",
                           filePath.c_str(), content.size());
            return false;
        }

        // Publish only a fully parsed catalog; entries merge over the existing table as before.
        for (auto& [key, value] : parsedEntries)
        {
            m_entries.insert_or_assign(key, std::move(value));
        }
        return true;
    }

    void StringTable::SetEntry(const std::string& key, const std::string& value)
    {
        m_entries[key] = value;
    }

    std::string StringTable::GetEntry(const std::string& key) const
    {
        auto it = m_entries.find(key);
        if (it != m_entries.end())
        {
            return it->second;
        }
        // Return a copy of the key itself as the missing-entry fallback.
        return key;
    }

    bool StringTable::HasEntry(const std::string& key) const
    {
        return m_entries.count(key) > 0;
    }

    std::vector<std::string> StringTable::GetAllKeys() const
    {
        std::vector<std::string> keys;
        keys.reserve(m_entries.size());
        for (const auto& [key, value] : m_entries)
        {
            keys.push_back(key);
        }
        std::sort(keys.begin(), keys.end());
        return keys;
    }
} // namespace Spark
