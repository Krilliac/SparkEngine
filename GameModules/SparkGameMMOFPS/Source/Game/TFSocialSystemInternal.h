/**
 * @file TFSocialSystemInternal.h
 * @brief Shared internals for the TFSocialSystem*.cpp split parts: wall-clock
 *        millisecond helper, ASCII case-insensitive name compare, the
 *        fixed-size wire-name copy/read helpers and the name trimmer. Include
 *        only from the TFSocialSystem translation units.
 */
#pragma once

#include "Net/TFSocialProtocol.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace Terrafront
{
    namespace SocialDetail
    {

        inline int64_t NowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        /// ASCII case-insensitive name compare (character names are alnum+space).
        inline bool NameEq(const std::string& a, const std::string& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
            {
                if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                    return false;
            }
            return true;
        }

        inline void CopyName(char (&out)[kTFSocialNameLen], const std::string& name)
        {
            const size_t n = std::min(name.size(), kTFSocialNameLen - 1);
            std::memcpy(out, name.data(), n);
            out[n] = '\0';
        }

        inline std::string NameFromWire(const char* field)
        {
            return std::string(field, strnlen(field, kTFSocialNameLen));
        }

        inline std::string TrimmedName(const std::string& raw)
        {
            size_t begin = 0, end = raw.size();
            while (begin < end && std::isspace(static_cast<unsigned char>(raw[begin])))
                ++begin;
            while (end > begin && std::isspace(static_cast<unsigned char>(raw[end - 1])))
                --end;
            return raw.substr(begin, end - begin);
        }

    } // namespace SocialDetail
} // namespace Terrafront
