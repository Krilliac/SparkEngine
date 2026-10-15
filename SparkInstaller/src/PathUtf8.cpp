#include "PathUtf8.h"

#include <climits>
#include <cstddef>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SparkInstaller
{
#ifdef _WIN32
    namespace
    {
        // flags = MB_ERR_INVALID_CHARS makes ill-formed input fail instead of
        // being replaced.
        bool WideFromUtf8(std::string_view utf8, DWORD flags, std::wstring& wide)
        {
            wide.clear();
            if (utf8.empty())
            {
                return true;
            }
            if (utf8.size() > static_cast<std::size_t>(INT_MAX))
            {
                return false;
            }
            const int inputSize = static_cast<int>(utf8.size());
            const int size = ::MultiByteToWideChar(CP_UTF8, flags, utf8.data(), inputSize, nullptr, 0);
            if (size <= 0)
            {
                return false;
            }
            wide.resize(static_cast<std::size_t>(size));
            return ::MultiByteToWideChar(CP_UTF8, flags, utf8.data(), inputSize, wide.data(), size) == size;
        }
    } // namespace

    bool ContainsDbcsBackslashTrail(std::string_view bytes, unsigned int codePage)
    {
        for (std::size_t index = 0; index < bytes.size(); ++index)
        {
            const auto byte = static_cast<BYTE>(bytes[index]);
            if (::IsDBCSLeadByteEx(codePage, byte))
            {
                // A lead byte always consumes the next byte as its trail.
                if (index + 1 < bytes.size() && bytes[index + 1] == '\\')
                {
                    return true;
                }
                ++index;
            }
        }
        return false;
    }

    std::string Utf8FromWide(std::wstring_view wide)
    {
        if (wide.empty() || wide.size() > static_cast<std::size_t>(INT_MAX))
        {
            return {};
        }
        const int inputSize = static_cast<int>(wide.size());
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), inputSize, nullptr, 0, nullptr, nullptr);
        if (size <= 0)
        {
            return {};
        }
        std::string utf8(static_cast<std::size_t>(size), '\0');
        if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(), inputSize, utf8.data(), size, nullptr, nullptr) != size)
        {
            return {};
        }
        return utf8;
    }
#endif

    bool IsValidUtf8(std::string_view text)
    {
#ifdef _WIN32
        std::wstring ignored;
        return WideFromUtf8(text, MB_ERR_INVALID_CHARS, ignored);
#else
        std::size_t index = 0;
        while (index < text.size())
        {
            const auto lead = static_cast<unsigned char>(text[index]);
            std::size_t length = 0;
            unsigned char minSecond = 0x80;
            unsigned char maxSecond = 0xBF;
            if (lead < 0x80)
            {
                ++index;
                continue;
            }
            if (lead >= 0xC2 && lead <= 0xDF)
            {
                length = 2;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                length = 3;
                minSecond = lead == 0xE0 ? 0xA0 : 0x80; // no overlongs
                maxSecond = lead == 0xED ? 0x9F : 0xBF; // no surrogates
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                length = 4;
                minSecond = lead == 0xF0 ? 0x90 : 0x80; // no overlongs
                maxSecond = lead == 0xF4 ? 0x8F : 0xBF; // nothing above U+10FFFF
            }
            else
            {
                return false;
            }
            if (text.size() - index < length)
            {
                return false;
            }
            const auto second = static_cast<unsigned char>(text[index + 1]);
            if (second < minSecond || second > maxSecond)
            {
                return false;
            }
            for (std::size_t offset = 2; offset < length; ++offset)
            {
                const auto continuation = static_cast<unsigned char>(text[index + offset]);
                if (continuation < 0x80 || continuation > 0xBF)
                {
                    return false;
                }
            }
            index += length;
        }
        return true;
#endif
    }

    std::filesystem::path PathFromUtf8(std::string_view utf8)
    {
#ifdef _WIN32
        std::wstring wide;
        WideFromUtf8(utf8, 0, wide);
        return std::filesystem::path(std::move(wide));
#else
        // POSIX paths are byte strings; the installer's bytes are UTF-8.
        return std::filesystem::path(std::string(utf8));
#endif
    }

    std::string PathToUtf8(const std::filesystem::path& path)
    {
#ifdef _WIN32
        return Utf8FromWide(path.native());
#else
        return path.native();
#endif
    }

    bool ProcessArgFromUtf8(std::string_view utf8, std::string& narrow)
    {
#ifdef _WIN32
        std::wstring wide;
        if (!WideFromUtf8(utf8, MB_ERR_INVALID_CHARS, wide))
        {
            return false;
        }
        if (wide.empty())
        {
            narrow.clear();
            return true;
        }
        const int inputSize = static_cast<int>(wide.size());
        if (::GetACP() == CP_UTF8)
        {
            // A UTF-8 process code page (activeCodePage manifest or the
            // system-wide beta option) represents everything; it also rejects
            // the best-fit flag and a used-default pointer.
            const int size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), inputSize, nullptr, 0,
                                                   nullptr, nullptr);
            if (size <= 0)
            {
                return false;
            }
            std::string converted(static_cast<std::size_t>(size), '\0');
            if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), inputSize, converted.data(), size,
                                      nullptr, nullptr) != size)
            {
                return false;
            }
            narrow = std::move(converted);
            return true;
        }
        // WC_NO_BEST_FIT_CHARS stops "best fit" substitutions (e.g. a Cyrillic
        // letter becoming a look-alike Latin one), which would name a
        // different directory; the default-character flag reports any loss.
        BOOL usedDefault = FALSE;
        const int size = ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), inputSize, nullptr, 0,
                                               nullptr, &usedDefault);
        if (size <= 0 || usedDefault)
        {
            return false;
        }
        std::string converted(static_cast<std::size_t>(size), '\0');
        if (::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), inputSize, converted.data(), size, nullptr,
                                  &usedDefault) != size ||
            usedDefault)
        {
            return false;
        }
        // SparkBuild's quoting doubles backslashes byte by byte, which would
        // corrupt a double-byte character whose trail byte is 0x5C.
        if (ContainsDbcsBackslashTrail(converted, ::GetACP()))
        {
            return false;
        }
        narrow = std::move(converted);
        return true;
#else
        // POSIX passes path bytes through unchanged.
        narrow.assign(utf8.data(), utf8.size());
        return true;
#endif
    }
} // namespace SparkInstaller
