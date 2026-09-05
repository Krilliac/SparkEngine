/**
 * @file WindowsCommandLine.h
 * @brief Win32 command-line token helpers shared by engine startup surfaces.
 */

#pragma once

#ifdef SPARK_PLATFORM_WINDOWS

#include <climits>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
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

    /**
     * @brief Whole-token numeric parse.
     *
     * Returns nullopt unless the entire token is consumed, so a value such as
     * "12x" or "abc" is rejected instead of silently reading as 12 or 0.
     */
    template <typename ValueType> std::optional<ValueType> ParseWholeWideNumber(const std::wstring& token)
    {
        static_assert(std::is_arithmetic_v<ValueType>, "ParseWholeWideNumber handles numeric options only");
        if (token.empty())
            return std::nullopt;

        size_t consumed = 0;
        ValueType value{};
        try
        {
            if constexpr (std::is_floating_point_v<ValueType>)
                value = static_cast<ValueType>(std::stod(token, &consumed));
            else
                value = static_cast<ValueType>(std::stoll(token, &consumed));
        }
        catch (const std::exception&)
        {
            return std::nullopt;
        }

        if (consumed != token.size())
            return std::nullopt;
        return value;
    }

    /**
     * @brief Whole-token numeric value of an exact command-line option.
     *
     * The engine's numeric flags (-test-frames, -threads, -test-seconds) used to
     * be read with std::wstring::find plus a fixed offset, which matched inside
     * unrelated arguments (`-threadsafe`, a module path) and accepted trailing
     * garbage. Exact tokenization plus a whole-token parse removes both.
     */
    template <typename ValueType>
    std::optional<ValueType> FindWindowsCommandLineNumber(const wchar_t* commandLine, std::wstring_view option)
    {
        const auto token = FindWindowsCommandLineArgument(commandLine, option);
        if (!token)
            return std::nullopt;
        return ParseWholeWideNumber<ValueType>(*token);
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
