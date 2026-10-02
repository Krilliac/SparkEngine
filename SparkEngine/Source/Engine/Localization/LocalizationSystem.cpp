/**
 * @file LocalizationSystem.cpp
 * @brief Implementation of the localization and string table system
 */

#include "LocalizationSystem.h"
#include "../../Core/FaultIsolation.h"
#include "../../Utils/ContainerUtils.h"
#include "../../Utils/Validate.h"

#include <sstream>
#include <algorithm>
#include <charconv>

namespace Spark
{

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
