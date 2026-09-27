/**
 * @file ShowcaseLocalization.cpp
 * @brief Validated loading and lookup of the SparkGame showcase string tables
 */

#include "ShowcaseLocalization.h"

#include "Engine/Localization/LocalizationSystem.h"

#include <utility>

namespace ShowcaseLocalization
{

    bool LoadShowcaseStrings(Spark::LocalizationSystem& localization, const std::filesystem::path& root,
                             std::string* error)
    {
        auto fail = [error](std::string message)
        {
            if (error)
                *error = std::move(message);
            return false;
        };

        for (const auto& language : kShowcaseLanguageFiles)
        {
            const std::string path = (root / language[1]).string();
            Spark::StringTable table;
            if (!table.LoadFromFile(path))
                return fail(std::string("cannot load showcase strings from ") + language[1]);
            for (const ShowcaseString& entry : kShowcaseStrings)
            {
                if (!table.HasEntry(entry.key))
                    return fail(std::string(language[1]) + " is missing key " + entry.key);
            }
        }

        for (const auto& language : kShowcaseLanguageFiles)
        {
            if (!localization.LoadLanguage(language[0], (root / language[1]).string()))
                return fail(std::string("localization system rejected ") + language[1]);
        }
        return true;
    }

    std::string ShowcaseText(const Spark::LocalizationSystem* localization, std::string_view key)
    {
        const std::string keyString(key);
        if (localization)
        {
            std::string text = localization->GetString(keyString);
            if (text != keyString)
                return text;
        }
        for (const ShowcaseString& entry : kShowcaseStrings)
        {
            if (key == entry.key)
                return entry.english;
        }
        return keyString;
    }

} // namespace ShowcaseLocalization
