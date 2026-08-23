/**
 * @file SecureRandom.cpp
 * @brief Operating-system-backed cryptographic random byte generation.
 */
#include "SecureRandom.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#endif

namespace Spark::SecureRandom
{
    bool Fill(void* buffer, size_t size) noexcept
    {
        if (size == 0)
            return true;
        if (!buffer)
            return false;

        auto* output = static_cast<uint8_t*>(buffer);
#if defined(_WIN32)
        while (size > 0)
        {
            const ULONG chunk =
                static_cast<ULONG>(std::min(size, static_cast<size_t>((std::numeric_limits<ULONG>::max)())));
            if (BCryptGenRandom(nullptr, output, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
                return false;
            output += chunk;
            size -= chunk;
        }
        return true;
#elif defined(__APPLE__)
        arc4random_buf(output, size);
        return true;
#elif defined(__linux__)
        while (size > 0)
        {
            const ssize_t count = getrandom(output, size, 0);
            if (count < 0)
            {
                if (errno == EINTR)
                    continue;
                return false;
            }
            if (count == 0)
                return false;
            output += static_cast<size_t>(count);
            size -= static_cast<size_t>(count);
        }
        return true;
#else
        (void)output;
        return false;
#endif
    }

    std::string HexToken(size_t byteCount)
    {
        if (byteCount == 0 || byteCount > 1024)
            return {};
        std::vector<uint8_t> bytes(byteCount);
        if (!Fill(bytes.data(), bytes.size()))
            return {};

        static constexpr char digits[] = "0123456789abcdef";
        std::string token(byteCount * 2, '\0');
        for (size_t i = 0; i < byteCount; ++i)
        {
            token[i * 2] = digits[bytes[i] >> 4];
            token[i * 2 + 1] = digits[bytes[i] & 0x0f];
        }
        return token;
    }
} // namespace Spark::SecureRandom
