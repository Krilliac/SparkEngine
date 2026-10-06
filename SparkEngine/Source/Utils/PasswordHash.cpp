/**
 * @file PasswordHash.cpp
 * @brief libsodium HMAC-SHA256 and PBKDF2 password hashing.
 */
#include "PasswordHash.h"
#include "ScopeGuard.h"
#include "SecureRandom.h"
#include "SecureMemory.h"

#include <sodium.h>

#ifndef SPARK_HAS_LIBSODIUM
#error "PasswordHash requires libsodium"
#endif

#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace Spark::PasswordHash
{
    namespace
    {
        using Digest = Sha256Digest;
        constexpr uint32_t kIterations = 600000;
        constexpr uint32_t kMinimumIterations = 600000;
        constexpr uint32_t kMaximumIterations = 1000000;
        constexpr size_t kSaltBytes = 16;
        constexpr size_t kDerivedBytes = 32;
        constexpr size_t kMaximumPasswordBytes = 1024;
        constexpr size_t kMaximumEncodedBytes = 256;
        constexpr std::string_view kScheme = "pbkdf2-sha256";

        // The keyed libsodium state contains both pre-absorbed HMAC pads. Copying it for
        // each PBKDF2 round avoids rebuilding the key schedule and leaves only the two
        // SHA-256 compressions needed for a 32-byte round input.
        using HmacSha256Key = crypto_auth_hmacsha256_state;

        void RequireSodium(int result)
        {
            if (result < 0)
            {
                throw std::runtime_error("libsodium HMAC-SHA256 operation failed");
            }
        }

        void PrepareHmacSha256Key(HmacSha256Key& prepared, const uint8_t* key, size_t keyLength)
        {
            RequireSodium(sodium_init());
            RequireSodium(crypto_auth_hmacsha256_init(&prepared, key, keyLength));
        }

        Digest HmacSha256(const HmacSha256Key& key, const uint8_t* data, size_t dataLength)
        {
            HmacSha256Key state = key;
            const auto clearState = Spark::MakeScopeExit([&] { SecureErase(&state, sizeof(state)); });
            if (dataLength > 0)
            {
                RequireSodium(crypto_auth_hmacsha256_update(&state, data, dataLength));
            }
            Digest output{};
            if (crypto_auth_hmacsha256_final(&state, output.data()) < 0)
            {
                SecureErase(output.data(), output.size());
                throw std::runtime_error("libsodium HMAC-SHA256 operation failed");
            }
            return output;
        }

        std::vector<uint8_t> Derive(std::string_view password, const std::vector<uint8_t>& salt, uint32_t iterations,
                                    size_t derivedLength)
        {
            constexpr size_t hashLength = 32;
            std::vector<uint8_t> derived;
            const auto clearDerivedOnFailure =
                Spark::MakeScopeFail([&] { SecureErase(derived.data(), derived.size()); });
            derived.reserve(derivedLength);
            const auto* key = reinterpret_cast<const uint8_t*>(password.data());
            // PBKDF2's HMAC key normalization must happen once, not once per
            // iteration. Re-hashing long passwords in every round creates a
            // password-length-amplified denial-of-service path.
            HmacSha256Key preparedKey{};
            const auto clearPreparedKey = Spark::MakeScopeExit([&] { SecureErase(&preparedKey, sizeof(preparedKey)); });
            PrepareHmacSha256Key(preparedKey, key, password.size());
            for (uint32_t block = 1; derived.size() < derivedLength; ++block)
            {
                std::vector<uint8_t> input = salt;
                input.push_back(static_cast<uint8_t>(block >> 24));
                input.push_back(static_cast<uint8_t>(block >> 16));
                input.push_back(static_cast<uint8_t>(block >> 8));
                input.push_back(static_cast<uint8_t>(block));
                Digest value = HmacSha256(preparedKey, input.data(), input.size());
                const auto clearValue = Spark::MakeScopeExit([&] { SecureErase(value.data(), value.size()); });
                Digest accumulated = value;
                const auto clearAccumulated =
                    Spark::MakeScopeExit([&] { SecureErase(accumulated.data(), accumulated.size()); });
                for (uint32_t round = 1; round < iterations; ++round)
                {
                    value = HmacSha256(preparedKey, value.data(), value.size());
                    for (size_t i = 0; i < accumulated.size(); ++i)
                        accumulated[i] ^= value[i];
                }
                const size_t count = std::min(hashLength, derivedLength - derived.size());
                derived.insert(derived.end(), accumulated.begin(), accumulated.begin() + count);
            }
            return derived;
        }

        std::string ToHex(const std::vector<uint8_t>& bytes)
        {
            static constexpr char digits[] = "0123456789abcdef";
            std::string output(bytes.size() * 2, '\0');
            for (size_t i = 0; i < bytes.size(); ++i)
            {
                output[i * 2] = digits[bytes[i] >> 4];
                output[i * 2 + 1] = digits[bytes[i] & 0x0f];
            }
            return output;
        }

        int HexNibble(char value)
        {
            if (value >= '0' && value <= '9')
                return value - '0';
            if (value >= 'a' && value <= 'f')
                return value - 'a' + 10;
            if (value >= 'A' && value <= 'F')
                return value - 'A' + 10;
            return -1;
        }

        bool FromHex(std::string_view text, std::vector<uint8_t>& bytes)
        {
            if (text.empty() || text.size() % 2 != 0)
                return false;
            bytes.clear();
            bytes.reserve(text.size() / 2);
            for (size_t i = 0; i < text.size(); i += 2)
            {
                const int high = HexNibble(text[i]);
                const int low = HexNibble(text[i + 1]);
                if (high < 0 || low < 0)
                    return false;
                bytes.push_back(static_cast<uint8_t>((high << 4) | low));
            }
            return true;
        }

        bool ConstantTimeEqual(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right)
        {
            if (left.size() != right.size())
                return false;
            uint8_t difference = 0;
            for (size_t i = 0; i < left.size(); ++i)
                difference |= static_cast<uint8_t>(left[i] ^ right[i]);
            return difference == 0;
        }

        std::vector<std::string_view> Split(std::string_view encoded)
        {
            std::vector<std::string_view> parts;
            size_t start = 0;
            while (true)
            {
                const size_t separator = encoded.find('$', start);
                parts.push_back(
                    encoded.substr(start, separator == std::string_view::npos ? separator : separator - start));
                if (separator == std::string_view::npos)
                    break;
                start = separator + 1;
            }
            return parts;
        }
    } // namespace

    Sha256Digest ComputeHmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> data)
    {
        HmacSha256Key prepared{};
        const auto clearPrepared = Spark::MakeScopeExit([&] { SecureErase(&prepared, sizeof(prepared)); });
        PrepareHmacSha256Key(prepared, key.data(), key.size());
        return HmacSha256(prepared, data.data(), data.size());
    }

    std::string Create(std::string_view password)
    {
        if (password.size() > kMaximumPasswordBytes)
            return {};
        std::vector<uint8_t> salt(kSaltBytes);
        if (!SecureRandom::Fill(salt.data(), salt.size()))
            return {};
        std::vector<uint8_t> derived = Derive(password, salt, kIterations, kDerivedBytes);
        const auto clearDerived = Spark::MakeScopeExit([&] { SecureErase(derived.data(), derived.size()); });
        return std::string(kScheme) + '$' + std::to_string(kIterations) + '$' + ToHex(salt) + '$' + ToHex(derived);
    }

    bool Verify(std::string_view password, std::string_view encodedHash)
    {
        if (password.size() > kMaximumPasswordBytes || encodedHash.size() > kMaximumEncodedBytes)
            return false;
        const std::vector<std::string_view> parts = Split(encodedHash);
        if (parts.size() != 4 || parts[0] != kScheme)
            return false;
        uint32_t iterations = 0;
        const auto parse = std::from_chars(parts[1].data(), parts[1].data() + parts[1].size(), iterations);
        if (parse.ec != std::errc{} || parse.ptr != parts[1].data() + parts[1].size() ||
            iterations < kMinimumIterations || iterations > kMaximumIterations)
            return false;
        std::vector<uint8_t> salt;
        std::vector<uint8_t> expected;
        if (!FromHex(parts[2], salt) || salt.size() != kSaltBytes || !FromHex(parts[3], expected) ||
            expected.size() != kDerivedBytes)
            return false;
        std::vector<uint8_t> actual = Derive(password, salt, iterations, expected.size());
        const auto clearActual = Spark::MakeScopeExit([&] { SecureErase(actual.data(), actual.size()); });
        return ConstantTimeEqual(actual, expected);
    }
} // namespace Spark::PasswordHash
