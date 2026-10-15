/**
 * @file EditorPreferencesPath.cpp
 * @brief Location of the per-user editor-preferences.ini.
 *
 * Kept apart from EditorPreferences.cpp so the libc++ fuzz binding of the parser does not pull
 * in ProjectManager.
 */

#include "EditorPreferences.h"

#include "ProjectManager.h"

#include <string>

namespace SparkEditor
{
    std::filesystem::path EditorPreferencesPath()
    {
        // GetEditorDataDirectory returns UTF-8; build the path from code units so non-ASCII
        // profile directories survive on Windows.
        const std::string directory = ProjectManager::GetEditorDataDirectory();
        const std::u8string utf8(directory.begin(), directory.end());
        return std::filesystem::path(utf8) / "editor-preferences.ini";
    }
} // namespace SparkEditor
