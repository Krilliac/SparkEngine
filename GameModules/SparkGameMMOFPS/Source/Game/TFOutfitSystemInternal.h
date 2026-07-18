/**
 * @file TFOutfitSystemInternal.h
 * @brief Shared internals for the TFOutfitSystem*.cpp split parts: wall-clock
 *        millisecond helper, case-insensitive name compare and the fixed-size
 *        wire-field copy/read helpers. Include only from the TFOutfitSystem
 *        translation units.
 */
#pragma once

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace Terrafront
{
    namespace OutfitDetail
    {

        inline int64_t NowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
        }

        inline bool EqualsNoCase(const std::string& a, const std::string& b)
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

        inline void CopyField(char* dst, size_t dstSize, const std::string& src)
        {
            std::strncpy(dst, src.c_str(), dstSize - 1);
            dst[dstSize - 1] = '\0';
        }

        inline std::string FieldToString(const char* field, size_t fieldSize)
        {
            return std::string(field, strnlen(field, fieldSize));
        }

    } // namespace OutfitDetail
} // namespace Terrafront
