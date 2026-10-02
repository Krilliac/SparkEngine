/**
 * @file TFCrypto.cpp
 * @brief libsodium SHA-256 / HMAC-SHA256 with PBKDF2-HMAC-SHA256 composition.
 */
#include "Account/TFCrypto.h"

#include "Utils/SecureMemory.h"

#include <sodium.h>

#ifndef SPARK_HAS_LIBSODIUM
#error "TFCrypto requires libsodium"
#endif

#include <algorithm>
#include <stdexcept>

namespace Terrafront::Crypto
{

    namespace
    {

        // One non-elidable bulk erase (Utils/SecureMemory.h): the PBKDF2 loop below
        // erases its schedule and intermediate states every round.
        using Spark::SecureErase;

        class EraseOnExit
        {
          public:
            EraseOnExit(void* data, size_t size) noexcept : m_data(data), m_size(size) {}
            ~EraseOnExit() { SecureErase(m_data, m_size); }

            EraseOnExit(const EraseOnExit&) = delete;
            EraseOnExit& operator=(const EraseOnExit&) = delete;

          private:
            void* m_data;
            size_t m_size;
        };

        class EraseVectorOnFailure
        {
          public:
            explicit EraseVectorOnFailure(std::vector<uint8_t>& value) noexcept : m_value(value) {}
            ~EraseVectorOnFailure()
            {
                if (m_armed && !m_value.empty())
                    SecureErase(m_value.data(), m_value.size());
            }

            EraseVectorOnFailure(const EraseVectorOnFailure&) = delete;
            EraseVectorOnFailure& operator=(const EraseVectorOnFailure&) = delete;

            void Release() noexcept { m_armed = false; }

          private:
            std::vector<uint8_t>& m_value;
            bool m_armed = true;
        };

        // libsodium keeps both keyed HMAC pad states after init. A per-round copy
        // preserves the existing PBKDF2 key-schedule optimization.
        using HmacSha256Key = crypto_auth_hmacsha256_state;

        void RequireSodium(int result)
        {
            if (result < 0)
            {
                throw std::runtime_error("libsodium SHA-256 operation failed");
            }
        }

        void PrepareHmacSha256Key(HmacSha256Key& prepared, const uint8_t* key, size_t keyLen)
        {
            RequireSodium(sodium_init());
            RequireSodium(crypto_auth_hmacsha256_init(&prepared, key, keyLen));
        }

        Sha256Digest HmacSha256WithKey(const HmacSha256Key& prepared, const uint8_t* data, size_t dataLen)
        {
            HmacSha256Key state = prepared;
            const EraseOnExit clearState(&state, sizeof(state));
            if (dataLen > 0)
            {
                RequireSodium(crypto_auth_hmacsha256_update(&state, data, dataLen));
            }
            Sha256Digest output{};
            if (crypto_auth_hmacsha256_final(&state, output.data()) < 0)
            {
                SecureErase(output.data(), output.size());
                throw std::runtime_error("libsodium HMAC-SHA256 operation failed");
            }
            return output;
        }

        int HexNibble(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

    } // namespace

    Sha256Digest Sha256(const uint8_t* data, size_t len)
    {
        RequireSodium(sodium_init());
        crypto_hash_sha256_state state{};
        const EraseOnExit clearState(&state, sizeof(state));
        RequireSodium(crypto_hash_sha256_init(&state));
        if (len > 0)
        {
            RequireSodium(crypto_hash_sha256_update(&state, data, len));
        }
        Sha256Digest output{};
        RequireSodium(crypto_hash_sha256_final(&state, output.data()));
        return output;
    }

    Sha256Digest Sha256(const std::string& data)
    {
        return Sha256(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    Sha256Digest HmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen)
    {
        HmacSha256Key prepared;
        const EraseOnExit clearPrepared(&prepared, sizeof(prepared));
        PrepareHmacSha256Key(prepared, key, keyLen);
        return HmacSha256WithKey(prepared, data, dataLen);
    }

    Sha256Digest HmacSha256(const std::string& key, const std::string& data)
    {
        return HmacSha256(reinterpret_cast<const uint8_t*>(key.data()), key.size(),
                          reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    std::vector<uint8_t> Pbkdf2HmacSha256(const std::string& password, const std::vector<uint8_t>& salt,
                                          uint32_t iterations, size_t dkLen)
    {
        constexpr size_t kHLen = 32;
        std::vector<uint8_t> dk;
        EraseVectorOnFailure clearDerivedKeyOnFailure(dk);
        dk.reserve(dkLen);

        HmacSha256Key prepared;
        const EraseOnExit clearPrepared(&prepared, sizeof(prepared));
        PrepareHmacSha256Key(prepared, reinterpret_cast<const uint8_t*>(password.data()), password.size());
        const uint32_t blockCount = static_cast<uint32_t>((dkLen + kHLen - 1) / kHLen);

        for (uint32_t i = 1; i <= blockCount; ++i)
        {
            std::vector<uint8_t> saltIdx = salt;
            saltIdx.push_back(static_cast<uint8_t>(i >> 24));
            saltIdx.push_back(static_cast<uint8_t>(i >> 16));
            saltIdx.push_back(static_cast<uint8_t>(i >> 8));
            saltIdx.push_back(static_cast<uint8_t>(i));

            Sha256Digest u = HmacSha256WithKey(prepared, saltIdx.data(), saltIdx.size());
            Sha256Digest t = u;
            const EraseOnExit clearU(u.data(), u.size());
            const EraseOnExit clearT(t.data(), t.size());
            for (uint32_t round = 1; round < iterations; ++round)
            {
                Sha256Digest nextU = HmacSha256WithKey(prepared, u.data(), u.size());
                const EraseOnExit clearNextU(nextU.data(), nextU.size());
                u = nextU;
                for (size_t k = 0; k < t.size(); ++k)
                    t[k] ^= u[k];
            }

            const size_t take = std::min(kHLen, dkLen - dk.size());
            dk.insert(dk.end(), t.begin(), t.begin() + static_cast<std::ptrdiff_t>(take));
        }
        clearDerivedKeyOnFailure.Release();
        return dk;
    }

    std::string ToHex(const uint8_t* data, size_t len)
    {
        static const char kDigits[] = "0123456789abcdef";
        std::string out;
        out.resize(len * 2);
        for (size_t i = 0; i < len; ++i)
        {
            out[i * 2] = kDigits[(data[i] >> 4) & 0xF];
            out[i * 2 + 1] = kDigits[data[i] & 0xF];
        }
        return out;
    }

    std::string ToHex(const std::vector<uint8_t>& data)
    {
        return ToHex(data.data(), data.size());
    }

    std::vector<uint8_t> FromHex(const std::string& hex)
    {
        std::vector<uint8_t> out;
        if (hex.size() % 2 != 0)
            return out; // malformed (odd length) -> empty
        out.reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2)
        {
            const int hi = HexNibble(hex[i]);
            const int lo = HexNibble(hex[i + 1]);
            if (hi < 0 || lo < 0)
                return {}; // malformed (non-hex char) -> empty
            out.push_back(static_cast<uint8_t>((hi << 4) | lo));
        }
        return out;
    }

    bool ConstantTimeEquals(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b)
    {
        if (a.size() != b.size())
            return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<uint8_t>(a[i] ^ b[i]);
        return diff == 0;
    }

    bool ConstantTimeEquals(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
            diff |= static_cast<uint8_t>(a[i]) ^ static_cast<uint8_t>(b[i]);
        return diff == 0;
    }

    Sha256Digest XorBytes(const Sha256Digest& a, const Sha256Digest& b)
    {
        Sha256Digest out{};
        for (size_t i = 0; i < out.size(); ++i)
        {
            out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
        }
        return out;
    }

    bool ConstantTimeEquals(const Sha256Digest& a, const Sha256Digest& b)
    {
        uint8_t diff = 0;
        for (size_t i = 0; i < a.size(); ++i)
        {
            diff |= static_cast<uint8_t>(a[i] ^ b[i]);
        }
        return diff == 0;
    }

    std::string Base64Encode(const std::vector<uint8_t>& data)
    {
        static const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((data.size() + 2) / 3 * 4);
        for (size_t i = 0; i < data.size(); i += 3)
        {
            const size_t remaining = data.size() - i;
            uint32_t group = static_cast<uint32_t>(data[i]) << 16;
            if (remaining > 1)
            {
                group |= static_cast<uint32_t>(data[i + 1]) << 8;
            }
            if (remaining > 2)
            {
                group |= data[i + 2];
            }
            out.push_back(kAlphabet[(group >> 18) & 0x3F]);
            out.push_back(kAlphabet[(group >> 12) & 0x3F]);
            out.push_back(remaining > 1 ? kAlphabet[(group >> 6) & 0x3F] : '=');
            out.push_back(remaining > 2 ? kAlphabet[group & 0x3F] : '=');
        }
        return out;
    }

    ScramKeys::~ScramKeys()
    {
        SecureErase(clientKey.data(), clientKey.size());
        SecureErase(storedKey.data(), storedKey.size());
        SecureErase(serverKey.data(), serverKey.size());
    }

    ScramKeys DeriveScramKeysFromSaltedPassword(const std::vector<uint8_t>& saltedPassword)
    {
        static const std::string kClientKeyLabel = "Client Key";
        static const std::string kServerKeyLabel = "Server Key";
        ScramKeys keys;
        keys.clientKey = HmacSha256(saltedPassword.data(), saltedPassword.size(),
                                    reinterpret_cast<const uint8_t*>(kClientKeyLabel.data()), kClientKeyLabel.size());
        keys.storedKey = Sha256(keys.clientKey.data(), keys.clientKey.size());
        keys.serverKey = HmacSha256(saltedPassword.data(), saltedPassword.size(),
                                    reinterpret_cast<const uint8_t*>(kServerKeyLabel.data()), kServerKeyLabel.size());
        return keys;
    }

    ScramKeys DeriveScramKeys(const std::string& password, const std::vector<uint8_t>& salt, uint32_t iterations)
    {
        std::vector<uint8_t> saltedPassword = Pbkdf2HmacSha256(password, salt, iterations, 32);
        const EraseOnExit clearSaltedPassword(saltedPassword.data(), saltedPassword.size());
        return DeriveScramKeysFromSaltedPassword(saltedPassword);
    }

    Sha256Digest ScramClientProof(const ScramKeys& keys, const std::string& authMessage)
    {
        Sha256Digest clientSignature =
            HmacSha256(keys.storedKey.data(), keys.storedKey.size(),
                       reinterpret_cast<const uint8_t*>(authMessage.data()), authMessage.size());
        const EraseOnExit clearSignature(clientSignature.data(), clientSignature.size());
        return XorBytes(keys.clientKey, clientSignature);
    }

} // namespace Terrafront::Crypto
