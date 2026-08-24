/**
 * @file WindowsCommandLine.h
 * @brief Win32 command-line token helpers shared by engine startup surfaces.
 */

#pragma once

#ifdef SPARK_PLATFORM_WINDOWS

#include <climits>
#include <optional>
#include <string>
#include <string_view>
#include <windows.h>
#include <shellapi.h>

namespace Spark::Platform
{
    /** @brief Return whether @p option appears as an exact command-line token. */
    inline bool HasWindowsCommandLineOption(const wchar_t* commandLine, std::wstring_view option)
    {
        if (!commandLine || option.empty())
            return false;

        std::wstring fullCommandLine = L"SparkEngine.exe";
        if (*commandLine != L'\0')
        {
            fullCommandLine.push_back(L' ');
            fullCommandLine.append(commandLine);
        }

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(fullCommandLine.c_str(), &argc);
        if (!argv)
            return false;

        bool found = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::wstring_view(argv[i]) == option)
            {
                found = true;
                break;
            }
        }
        LocalFree(argv);
        return found;
    }

    /**
     * @brief Find the value immediately following @p option in a wWinMain command line.
     *
     * @p commandLine is the lpCmdLine form (argv[0] omitted). CommandLineToArgvW
     * supplies the same quoting/backslash semantics users get in a normal Win32
     * process launch, so quoted paths, spaces, Unicode, and empty arguments are
     * handled without ad-hoc substring parsing.
     */
    inline std::optional<std::wstring> FindWindowsCommandLineArgument(const wchar_t* commandLine,
                                                                      std::wstring_view option)
    {
        if (!commandLine || option.empty())
            return std::nullopt;

        std::wstring fullCommandLine = L"SparkEngine.exe";
        if (*commandLine != L'\0')
        {
            fullCommandLine.push_back(L' ');
            fullCommandLine.append(commandLine);
        }

        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(fullCommandLine.c_str(), &argc);
        if (!argv)
            return std::nullopt;

        std::optional<std::wstring> value;
        for (int i = 1; i + 1 < argc; ++i)
        {
            if (std::wstring_view(argv[i]) == option)
            {
                value = argv[i + 1];
                break;
            }
        }
        LocalFree(argv);
        return value;
    }

    /** @brief UTF-8 form of FindWindowsCommandLineArgument for filesystem APIs. */
    inline std::optional<std::string> FindWindowsCommandLineUtf8Argument(const wchar_t* commandLine,
                                                                         std::wstring_view option)
    {
        const auto wide = FindWindowsCommandLineArgument(commandLine, option);
        if (!wide)
            return std::nullopt;
        if (wide->empty())
            return std::string{};
        if (wide->size() > static_cast<size_t>(INT_MAX))
            return std::nullopt;

        const int inputLength = static_cast<int>(wide->size());
        const int outputLength =
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide->data(), inputLength, nullptr, 0, nullptr, nullptr);
        if (outputLength <= 0)
            return std::nullopt;

        std::string utf8(static_cast<size_t>(outputLength), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide->data(), inputLength, utf8.data(), outputLength,
                                nullptr, nullptr) != outputLength)
        {
            return std::nullopt;
        }
        return utf8;
    }
} // namespace Spark::Platform

#endif // SPARK_PLATFORM_WINDOWS
