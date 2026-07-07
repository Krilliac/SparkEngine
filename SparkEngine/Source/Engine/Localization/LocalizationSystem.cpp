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
#include <regex>

namespace Spark
{

    // =============================================================================
    // StringTable
    // =============================================================================

    bool StringTable::LoadFromFile(const std::string& filePath)
    {
        std::ifstream file(filePath);
        if (!file.is_open())
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Core, "StringTable: failed to open localization file: %s",
                            filePath.c_str());
            return false;
        }

        // Simple JSON parser for flat key-value string maps.
        // Handles: { "key": "value", "key2": "value2" }
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        // Match quoted key/value pairs. The (?:[^"\\]|\\.)* body allows escaped
        // characters inside the strings (e.g. \" \\ \n), so a value such as
        // "He said \"hi\"" is captured whole instead of being truncated at the
        // first inner quote.
        std::regex kvRegex(R"~~("((?:[^"\\]|\\.)*)"\s*:\s*"((?:[^"\\]|\\.)*)")~~");
        auto begin = std::sregex_iterator(content.begin(), content.end(), kvRegex);
        auto end = std::sregex_iterator();

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
        for (auto it = begin; it != end; ++it)
        {
            const std::smatch& match = *it;
            m_entries[unescape(match[1].str())] = unescape(match[2].str());
            ++parsed;
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

    const std::string& StringTable::GetEntry(const std::string& key) const
    {
        auto it = m_entries.find(key);
        if (it != m_entries.end())
        {
            return it->second;
        }
        // Return the key parameter reference as fallback. Safe because callers
        // always pass an lvalue (string stored in m_entries or a local).
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
        std::lock_guard<std::mutex> lock(m_mutex);
        StringTable table;
        if (!table.LoadFromFile(filePath))
        {
            return false;
        }
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

    const std::string& LocalizationSystem::GetString(const std::string& key) const
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
        std::string result = tmpl;
        for (size_t i = 0; i < args.size(); ++i)
        {
            std::string placeholder = "{" + std::to_string(i) + "}";
            size_t pos = 0;
            while ((pos = result.find(placeholder, pos)) != std::string::npos)
            {
                result.replace(pos, placeholder.length(), args[i]);
                pos += args[i].length();
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
