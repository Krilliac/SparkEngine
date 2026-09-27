/**
 * @file LocalizationSystem.cpp
 * @brief Implementation of the localization and string table system
 */

#include "LocalizationSystem.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/ContainerUtils.h"
#include "../../Utils/Validate.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <charconv>
#include <filesystem>
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

        size_t parsed = 0;
        size_t pos = 0;
        while (true)
        {
            const size_t keyOpen = content.find('"', pos);
            if (keyOpen == std::string::npos)
                break;
            size_t keyClose = 0;
            if (!FindClosingQuote(content, keyOpen, keyClose))
                break; // unterminated string: nothing after it can pair up

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
                        break;
                    m_entries[unescape(content.substr(keyOpen + 1, keyClose - keyOpen - 1))] =
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

    // =============================================================================
    // LocalizationSystem
    // =============================================================================

    LocalizationSystem& LocalizationSystem::Get()
    {
        static LocalizationSystem instance;
        return instance;
    }

    bool LocalizationSystem::LoadLanguage(const std::string& languageCode, const std::string& filePath)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Loading language '%s' from '%s'", languageCode.c_str(),
                       filePath.c_str());
        // Read and parse without holding m_mutex; lookups from other threads only wait
        // for the final insert.
        StringTable table;
        if (!table.LoadFromFile(filePath))
        {
            return false;
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_languages[languageCode] = std::move(table);
        return true;
    }

    bool LocalizationSystem::SetCurrentLanguage(const std::string& languageCode)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Core);
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Setting current language to '%s'", languageCode.c_str());
        // Copy callbacks under the lock, then invoke outside to prevent deadlocks
        // if callbacks call back into LocalizationSystem.
        std::vector<std::function<void(const std::string&)>> callbacks;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!Spark::ContainerUtils::Contains(m_languages, languageCode))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Core,
                               "LocalizationSystem: requested language '%s' not loaded (%zu available)",
                               languageCode.c_str(), m_languages.size());
                return false;
            }
            m_currentLanguage = languageCode;
            callbacks = m_languageChangedCallbacks;
        }

        for (const auto& callback : callbacks)
        {
            SPARK_GUARDED_UPDATE("Localization:LanguageChanged", "Core", { callback(languageCode); });
        }
        return true;
    }

    std::vector<std::string> LocalizationSystem::GetAvailableLanguages() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::vector<std::string> languages;
        languages.reserve(m_languages.size());
        for (const auto& [code, table] : m_languages)
        {
            languages.push_back(code);
        }
        std::sort(languages.begin(), languages.end());
        return languages;
    }

    std::string LocalizationSystem::GetString(const std::string& key) const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        // Try current language first
        auto currentIt = m_languages.find(m_currentLanguage);
        if (currentIt != m_languages.end() && currentIt->second.HasEntry(key))
        {
            return currentIt->second.GetEntry(key);
        }

        // Fall back to fallback language
        if (m_currentLanguage != m_fallbackLanguage)
        {
            auto fallbackIt = m_languages.find(m_fallbackLanguage);
            if (fallbackIt != m_languages.end() && fallbackIt->second.HasEntry(key))
            {
                return fallbackIt->second.GetEntry(key);
            }
        }

        return key; // Return the key itself
    }

    std::string LocalizationSystem::FormatImpl(const std::string& tmpl, const std::vector<std::string>& args) const
    {
        // Single left-to-right scan of the original template. Substituted argument
        // text is never re-scanned, so an argument containing "{N}" (e.g. a
        // user-supplied name) stays literal instead of being treated as a directive.
        std::string result;
        result.reserve(tmpl.length());
        size_t pos = 0;
        while (pos < tmpl.length())
        {
            size_t open = tmpl.find('{', pos);
            if (open == std::string::npos)
            {
                result.append(tmpl, pos, std::string::npos);
                break;
            }
            result.append(tmpl, pos, open - pos);

            size_t digitEnd = open + 1;
            while (digitEnd < tmpl.length() && tmpl[digitEnd] >= '0' && tmpl[digitEnd] <= '9')
            {
                ++digitEnd;
            }

            size_t index = 0;
            auto [parseEnd, parseErr] = std::from_chars(tmpl.data() + open + 1, tmpl.data() + digitEnd, index);
            bool isPlaceholder = digitEnd > open + 1 && digitEnd < tmpl.length() && tmpl[digitEnd] == '}' &&
                                 parseErr == std::errc{} && parseEnd == tmpl.data() + digitEnd && index < args.size();
            if (isPlaceholder)
            {
                result += args[index];
                pos = digitEnd + 1;
            }
            else
            {
                // Not a substitutable placeholder — emit the brace literally.
                result += '{';
                pos = open + 1;
            }
        }
        return result;
    }

    void LocalizationSystem::OnLanguageChanged(std::function<void(const std::string&)> callback)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_languageChangedCallbacks.push_back(std::move(callback));
    }

    std::string LocalizationSystem::Console_GetStatus() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::ostringstream oss;
        oss << "=== Localization System Status ===\n";
        oss << "Current language: " << m_currentLanguage << "\n";
        oss << "Fallback language: " << m_fallbackLanguage << "\n";
        oss << "Loaded languages: " << m_languages.size() << "\n";
        for (const auto& [code, table] : m_languages)
        {
            oss << "  " << code << ": " << table.GetEntryCount() << " entries";
            if (code == m_currentLanguage)
            {
                oss << " (active)";
            }
            oss << "\n";
        }
        return oss.str();
    }

    std::string LocalizationSystem::Console_ListKeys() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_languages.find(m_currentLanguage);
        if (it == m_languages.end())
        {
            return "No language loaded.\n";
        }
        std::ostringstream oss;
        auto keys = it->second.GetAllKeys();
        oss << "Keys for '" << m_currentLanguage << "' (" << keys.size() << " entries):\n";
        for (const auto& key : keys)
        {
            oss << "  " << key << " = \"" << it->second.GetEntry(key) << "\"\n";
        }
        return oss.str();
    }

} // namespace Spark
