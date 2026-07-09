/**
 * @file FolderDialog.cpp
 * @brief Cross-platform native folder-picker dialog implementation.
 * @author Spark Engine Team
 * @date 2025
 */

#include "FolderDialog.h"

#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#else
#include <cstdlib>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace SparkEditor::Utils
{
#ifndef _WIN32
    namespace
    {
        bool RunFolderPicker(const std::string& executable, const std::vector<std::string>& arguments,
                             std::string& result)
        {
            int pipefd[2];
            if (pipe(pipefd) != 0)
                return false;

            pid_t pid = fork();
            if (pid == -1)
            {
                close(pipefd[0]);
                close(pipefd[1]);
                return false;
            }

            if (pid == 0)
            {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);

                std::vector<char*> argv;
                argv.reserve(arguments.size() + 2);
                argv.push_back(const_cast<char*>(executable.c_str()));
                for (const auto& argument : arguments)
                    argv.push_back(const_cast<char*>(argument.c_str()));
                argv.push_back(nullptr);

                execvp(executable.c_str(), argv.data());
                _exit(127);
            }

            close(pipefd[1]);
            result.clear();
            char buf[1024];
            while (true)
            {
                ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
                if (n <= 0)
                    break;
                buf[n] = '\0';
                result += buf;
            }
            close(pipefd[0]);

            int status = 0;
            waitpid(pid, &status, 0);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        }
    } // namespace
#endif

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
        std::string result;
        std::vector<std::string> zenityArgs = {"--file-selection", "--directory", "--title",
                                               title ? title : "Select Folder"};
        bool ok = RunFolderPicker("zenity", zenityArgs, result);
        if (!ok || result.empty())
        {
            const char* home = std::getenv("HOME");
            ok = RunFolderPicker("kdialog", {"--getexistingdirectory", home ? home : "."}, result);
        }

        while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
        {
            result.pop_back();
        }
        if (ok && !result.empty() && std::filesystem::is_directory(result))
        {
            outPath = result;
            return true;
        }
        return false;
#endif
    }
} // namespace SparkEditor::Utils
