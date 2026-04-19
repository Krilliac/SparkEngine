/**
 * @file FolderDialog.h
 * @brief Cross-platform native folder-picker dialog.
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include <string>

namespace SparkEditor::Utils
{
    /**
     * Opens a native folder-chooser dialog and writes the chosen path to outPath.
     * Windows: SHBrowseForFolderA. Linux/macOS: zenity, falling back to kdialog.
     * @return true on success, false if the user cancelled or no dialog backend is available.
     */
    bool BrowseForFolder(std::string& outPath, const char* title = "Select Folder");
} // namespace SparkEditor::Utils
