/**
 * @file ConfigParser.h
 * @brief INI-style configuration file parser
 * @author Spark Engine Team
 * @date 2025
 *
 * Reads and writes INI-format configuration files with sections,
 * key-value pairs, and comment support.
 *
 * ## Format
 * @code
 *   # Comment
 *   ; Also a comment
 *
 *   [Graphics]
 *   resolution_x = 1920
 *   resolution_y = 1080
 *   fullscreen = true
 *   vsync = false
 *
 *   [Audio]
 *   master_volume = 0.8
 *   music_volume = 0.6
 * @endcode
 *
 * ## Usage
 * @code
 *   Spark::ConfigParser cfg;
 *   cfg.Load("settings.ini");
 *
 *   int width = cfg.GetInt("Graphics", "resolution_x", 1920);
 *   bool fs = cfg.GetBool("Graphics", "fullscreen", false);
 *
 *   cfg.SetInt("Graphics", "resolution_x", 2560);
 *   cfg.Save("settings.ini");
 * @endcode
 */

#pragma once

#include <string>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <optional>
#include <utility>

#include "StringUtils.h"

namespace Spark
{

    class LocalFileCache;

    class ConfigParser
    {
      public:
        ConfigParser() = default;

        // =============================================================================
        // Load / Save
        // =============================================================================

        /// Load from file. Returns true on success.
        bool Load(const std::string& path)
        {
            std::ifstream file(path);
            if (!file.is_open())
                return false;
            std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (file.bad())
                return false;
            return LoadFromString(content);
        }

        /// Load from file via LocalFileCache (avoids redundant disk reads)
        bool Load(const std::string& path, LocalFileCache& cache);

        /// Parse from a string
        bool LoadFromString(const std::string& content)
        {
            // Parse into a temporary map so a malformed reload cannot destroy
            // the last known-good configuration.
            decltype(m_sections) parsedSections;
            std::string currentSection;
            std::istringstream stream(content);
            std::string line;
            bool firstLine = true;

            while (std::getline(stream, line))
            {
                if (firstLine && line.starts_with("\xEF\xBB\xBF"))
                    line.erase(0, 3);
                firstLine = false;

                line = Trim(line);
                if (line.empty())
                    continue;

                // Comment
                if (line[0] == '#' || line[0] == ';')
                    continue;

                // Section header
                if (line.front() == '[')
                {
                    if (line.back() != ']')
                        return false;

                    currentSection = Trim(line.substr(1, line.size() - 2));
                    if (currentSection.empty())
                        return false;

                    parsedSections[currentSection]; // Create section if not exists
                    continue;
                }

                // Key = Value
                auto eq = line.find('=');
                if (eq == std::string::npos)
                    return false;

                std::string key = Trim(line.substr(0, eq));
                if (key.empty())
                    return false;

                std::string value = Trim(line.substr(eq + 1));
                parsedSections[currentSection][key] = value;
            }

            m_sections = std::move(parsedSections);
            return true;
        }

        /// Save to file. Returns true on success.
        bool Save(const std::string& path) const
        {
            std::ofstream file(path, std::ios::out | std::ios::trunc);
            if (!file.is_open())
                return false;
            file << SaveToString();
            return file.good();
        }

        /// Save to file and update LocalFileCache
        bool Save(const std::string& path, LocalFileCache& cache) const;

        /// Serialize to string
        std::string SaveToString() const
        {
            std::ostringstream ss;

            // Write global (empty section name) keys first
            auto globalIt = m_sections.find("");
            if (globalIt != m_sections.end() && !globalIt->second.empty())
            {
                for (const auto& [key, value] : globalIt->second)
                {
                    ss << key << " = " << value << "\n";
                }
                ss << "\n";
            }

            // Write named sections
            for (const auto& [section, keys] : m_sections)
            {
                if (section.empty())
                    continue;
                ss << "[" << section << "]\n";
                for (const auto& [key, value] : keys)
                {
                    ss << key << " = " << value << "\n";
                }
                ss << "\n";
            }
            return ss.str();
        }

        // =============================================================================
        // Getters
        // =============================================================================

        std::string GetString(const std::string& section, const std::string& key,
                              const std::string& defaultValue = "") const
        {
            auto sit = m_sections.find(section);
            if (sit == m_sections.end())
                return defaultValue;
            auto kit = sit->second.find(key);
            if (kit == sit->second.end())
                return defaultValue;
            return kit->second;
        }

        int GetInt(const std::string& section, const std::string& key, int defaultValue = 0) const
        {
            std::string val = GetString(section, key, "");
            if (val.empty())
                return defaultValue;
            try
            {
                return std::stoi(val);
            }
            catch (...)
            {
                return defaultValue;
            }
        }

        float GetFloat(const std::string& section, const std::string& key, float defaultValue = 0.0f) const
        {
            std::string val = GetString(section, key, "");
            if (val.empty())
                return defaultValue;
            try
            {
                return std::stof(val);
            }
            catch (...)
            {
                return defaultValue;
            }
        }

        bool GetBool(const std::string& section, const std::string& key, bool defaultValue = false) const
        {
            std::string val = GetString(section, key, "");
            if (val.empty())
                return defaultValue;
            std::string lower = StringUtils::ToLower(val);
            if (lower == "true" || lower == "1" || lower == "yes" || lower == "on")
                return true;
            if (lower == "false" || lower == "0" || lower == "no" || lower == "off")
                return false;
            return defaultValue;
        }

        // =============================================================================
        // Setters
        // =============================================================================

        void SetString(const std::string& section, const std::string& key, const std::string& value)
        {
            m_sections[section][key] = value;
        }

        void SetInt(const std::string& section, const std::string& key, int value)
        {
            SetString(section, key, std::to_string(value));
        }

        void SetFloat(const std::string& section, const std::string& key, float value)
        {
            SetString(section, key, std::to_string(value));
        }

        void SetBool(const std::string& section, const std::string& key, bool value)
        {
            SetString(section, key, value ? "true" : "false");
        }

        // =============================================================================
        // Query
        // =============================================================================

        bool HasSection(const std::string& section) const { return m_sections.contains(section); }

        bool HasKey(const std::string& section, const std::string& key) const
        {
            auto sit = m_sections.find(section);
            if (sit == m_sections.end())
                return false;
            return sit->second.contains(key);
        }

        std::vector<std::string> GetSections() const
        {
            std::vector<std::string> result;
            for (const auto& [name, _] : m_sections)
            {
                result.push_back(name);
            }
            return result;
        }

        std::vector<std::string> GetKeys(const std::string& section) const
        {
            std::vector<std::string> result;
            auto sit = m_sections.find(section);
            if (sit == m_sections.end())
                return result;
            for (const auto& [key, _] : sit->second)
            {
                result.push_back(key);
            }
            return result;
        }

        /// Remove a key from a section
        bool RemoveKey(const std::string& section, const std::string& key)
        {
            auto sit = m_sections.find(section);
            if (sit == m_sections.end())
                return false;
            return sit->second.erase(key) > 0;
        }

        /// Remove an entire section
        bool RemoveSection(const std::string& section) { return m_sections.erase(section) > 0; }

        /// Clear all data
        void Clear() { m_sections.clear(); }

      private:
        static std::string Trim(const std::string& str) { return StringUtils::Trim(str); }

        /// section name -> (key -> value)
        std::map<std::string, std::map<std::string, std::string>> m_sections;
    };

} // namespace Spark
