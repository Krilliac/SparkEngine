/**
 * @file EngineSettingsConsole.cpp
 * @brief The settings_* console commands
 *
 * Kept apart from EngineSettings.cpp so the settings reader links without the console
 * (the SEC-120 fuzz target FuzzerTests/FuzzEngineSettings.cpp drives EngineSettings::Load).
 */

#include "EngineSettings.h"
#include "Utils/LogMacros.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// Console commands
// =============================================================================
void EngineSettings::RegisterConsoleCommands()
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Core, "Registering settings console commands");
    auto& console = Spark::SimpleConsole::GetInstance();

    console.RegisterCommand(
        "settings_get",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 2)
            {
                return "Usage: settings_get <section> <key>";
            }
            auto& settings = EngineSettings::GetInstance();
            std::string val = settings.GetValue(args[0], args[1]);
            if (val.empty())
            {
                return "Key not found: " + args[0] + "." + args[1];
            }
            return args[0] + "." + args[1] + " = " + val;
        },
        "Get a settings value", "Settings");

    console.RegisterCommand(
        "settings_set",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.size() < 3)
            {
                return "Usage: settings_set <section> <key> <value>";
            }
            auto& settings = EngineSettings::GetInstance();
            if (settings.SetValue(args[0], args[1], args[2]))
            {
                return "Set " + args[0] + "." + args[1] + " = " + args[2];
            }
            return "Failed to set " + args[0] + "." + args[1];
        },
        "Set a settings value (runtime editable)", "Settings");

    console.RegisterCommand(
        "settings_save",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            if (settings.Save())
            {
                return "Settings saved to " + settings.GetFilePath();
            }
            return "Failed to save settings";
        },
        "Save settings to disk", "Settings");

    console.RegisterCommand(
        "settings_reload",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            if (settings.Load(settings.GetFilePath()))
            {
                return "Settings reloaded from " + settings.GetFilePath();
            }
            return "Failed to reload settings";
        },
        "Reload settings from disk (applies changes at runtime)", "Settings");

    console.RegisterCommand(
        "settings_reset",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            settings.ResetToDefaults();
            return "Settings reset to defaults (use settings_save to persist)";
        },
        "Reset all settings to defaults", "Settings");

    console.RegisterCommand(
        "settings_list",
        [](const std::vector<std::string>& args) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            std::stringstream ss;

            std::vector<std::string> sections;
            if (!args.empty())
            {
                sections.push_back(args[0]);
            }
            else
            {
                sections = settings.GetSections();
            }

            for (const auto& section : sections)
            {
                ss << "[" << section << "]\n";
                auto keys = settings.GetKeys(section);
                for (const auto& key : keys)
                {
                    ss << "  " << key << " = " << settings.GetValue(section, key) << "\n";
                }
            }
            return ss.str();
        },
        "List all settings (or settings in a section)", "Settings");

    console.RegisterCommand(
        "settings_sections",
        [](const std::vector<std::string>&) -> std::string
        {
            auto& settings = EngineSettings::GetInstance();
            std::stringstream ss;
            ss << "Available sections:\n";
            for (const auto& section : settings.GetSections())
            {
                auto keys = settings.GetKeys(section);
                ss << "  [" << section << "] (" << keys.size() << " keys)\n";
            }
            return ss.str();
        },
        "List all settings sections", "Settings");

    console.RegisterCommand(
        "settings_search",
        [](const std::vector<std::string>& args) -> std::string
        {
            if (args.empty())
            {
                return "Usage: settings_search <pattern>";
            }
            auto& settings = EngineSettings::GetInstance();
            const std::string& pattern = args[0];
            // Convert to lowercase for case-insensitive search
            std::string lowerPattern = pattern;
            std::transform(lowerPattern.begin(), lowerPattern.end(), lowerPattern.begin(), ::tolower);

            std::stringstream ss;
            int count = 0;
            for (const auto& section : settings.GetSections())
            {
                auto keys = settings.GetKeys(section);
                for (const auto& key : keys)
                {
                    std::string lowerSection = section;
                    std::transform(lowerSection.begin(), lowerSection.end(), lowerSection.begin(), ::tolower);
                    std::string lowerKey = key;
                    std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);

                    if (lowerSection.contains(lowerPattern) || lowerKey.contains(lowerPattern))
                    {
                        ss << "  " << section << "." << key << " = " << settings.GetValue(section, key) << "\n";
                        count++;
                    }
                }
            }
            if (count == 0)
            {
                return "No settings matching '" + pattern + "'";
            }
            return "Found " + std::to_string(count) + " matching settings:\n" + ss.str();
        },
        "Search settings by name pattern", "Settings");
}
