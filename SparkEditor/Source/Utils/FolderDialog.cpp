/**
 * @file FolderDialog.cpp
 * @brief Cross-platform native folder-picker dialog implementation.
 * @author Spark Engine Team
 * @date 2025
 */

#include "FolderDialog.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdio>
#include <cstdlib>
#endif

namespace SparkEditor::Utils
{
    bool BrowseForFolder(std::string& outPath, const char* title)
    {
#ifdef _WIN32
        BROWSEINFOA bi = {};
        bi.lpszTitle = title;
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;

        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl)
        {
            char path[MAX_PATH];
            if (SHGetPathFromIDListA(pidl, path))
            {
                outPath = path;
                CoTaskMemFree(pidl);
                return true;
            }
            CoTaskMemFree(pidl);
        }
        return false;
#else
        std::string cmd = "zenity --file-selection --directory --title=\"";
        cmd += title ? title : "Select Folder";
        cmd += "\" 2>/dev/null";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe)
        {
            pipe = popen("kdialog --getexistingdirectory ~ 2>/dev/null", "r");
        }
        if (!pipe)
        {
            return false;
        }

        char buf[1024];
        std::string result;
        while (fgets(buf, sizeof(buf), pipe) != nullptr)
        {
            result += buf;
        }
        int status = pclose(pipe);
        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        {
            result.pop_back();
        }
        if (status == 0 && !result.empty() && std::filesystem::is_directory(result))
        {
            outPath = result;
            return true;
        }
        return false;
#endif
    }
} // namespace SparkEditor::Utils
