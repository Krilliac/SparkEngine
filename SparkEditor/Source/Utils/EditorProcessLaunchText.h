#pragma once

/**
 * @file EditorProcessLaunchText.h
 * @brief Shared Unicode and platform command-line quoting helpers.
 */

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace SparkEditor::Detail
{
    inline std::wstring QuoteWindowsArgument(std::wstring_view argument)
    {
        if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
            return std::wstring(argument);

        std::wstring quoted;
        quoted.reserve(argument.size() + 2);
        quoted.push_back(L'"');
        size_t backslashes = 0;
        for (const wchar_t ch : argument)
        {
            if (ch == L'\\')
            {
                ++backslashes;
                continue;
            }
            if (ch == L'"')
            {
                quoted.append(backslashes * 2 + 1, L'\\');
                quoted.push_back(L'"');
            }
            else
            {
                quoted.append(backslashes, L'\\');
                quoted.push_back(ch);
            }
            backslashes = 0;
        }
        quoted.append(backslashes * 2, L'\\');
        quoted.push_back(L'"');
        return quoted;
    }

    inline bool DecodeUtf8(std::string_view input, std::wstring& output)
    {
        output.clear();
        for (size_t index = 0; index < input.size();)
        {
            const uint8_t first = static_cast<uint8_t>(input[index]);
            uint32_t codePoint = 0;
            size_t continuationCount = 0;
            if (first < 0x80)
            {
                codePoint = first;
            }
            else if ((first & 0xe0u) == 0xc0u)
            {
                codePoint = first & 0x1fu;
                continuationCount = 1;
            }
            else if ((first & 0xf0u) == 0xe0u)
            {
                codePoint = first & 0x0fu;
                continuationCount = 2;
            }
            else if ((first & 0xf8u) == 0xf0u)
            {
                codePoint = first & 0x07u;
                continuationCount = 3;
            }
            else
            {
                return false;
            }

            if (index + continuationCount >= input.size())
                return false;
            for (size_t offset = 1; offset <= continuationCount; ++offset)
            {
                const uint8_t continuation = static_cast<uint8_t>(input[index + offset]);
                if ((continuation & 0xc0u) != 0x80u)
                    return false;
                codePoint = (codePoint << 6u) | (continuation & 0x3fu);
            }

            const uint32_t minimum = continuationCount == 0   ? 0
                                     : continuationCount == 1 ? 0x80u
                                     : continuationCount == 2 ? 0x800u
                                                              : 0x10000u;
            if (codePoint < minimum || codePoint > 0x10ffffu || (codePoint >= 0xd800u && codePoint <= 0xdfffu))
                return false;

#if WCHAR_MAX <= 0xffff
            if (codePoint <= 0xffffu)
            {
                output.push_back(static_cast<wchar_t>(codePoint));
            }
            else
            {
                codePoint -= 0x10000u;
                output.push_back(static_cast<wchar_t>(0xd800u + (codePoint >> 10u)));
                output.push_back(static_cast<wchar_t>(0xdc00u + (codePoint & 0x3ffu)));
            }
#else
            output.push_back(static_cast<wchar_t>(codePoint));
#endif
            index += continuationCount + 1;
        }
        return true;
    }

    inline std::wstring QuotePosixArgument(std::wstring_view argument)
    {
        std::wstring quoted = L"'";
        for (const wchar_t character : argument)
        {
            if (character == L'\'')
                quoted += L"'\\''";
            else
                quoted.push_back(character);
        }
        quoted.push_back(L'\'');
        return quoted;
    }
} // namespace SparkEditor::Detail
