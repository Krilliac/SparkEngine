/**
 * @file NetworkTrustStore.cpp
 * @brief Server identity file and known_hosts store (see NetworkTrustStore.h for the formats)
 */

#include "NetworkTrustStore.h"
#include "NetworkBindPolicy.h"
#include "../../Core/EngineSettings.h"
#include "../../Utils/LogMacros.h"
#include "../../Utils/SecureMemory.h"
#include "../../Utils/SecureRandom.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iterator>
#include <system_error>

#ifndef SPARK_HAS_LIBSODIUM
#error "NetworkTrustStore.cpp requires libsodium (cmake/SparkLibsodium.cmake links spark_sodium)"
#endif
#include <sodium.h>

namespace Spark::Net
{

    namespace
    {
        constexpr std::array<uint8_t, 8> kIdentityMagic = {'S', 'P', 'K', 'S', 'V', 'I', 'D', 0};
        constexpr size_t kMagicOffset = 0;
        constexpr size_t kVersionOffset = 8;
        constexpr size_t kReservedOffset = 12;
        constexpr size_t kSecretOffset = 16;
        constexpr size_t kChecksumOffset = kSecretOffset + SERVER_SIGNING_SECRET_SIZE;
        constexpr size_t kChecksumSize = 16;
        static_assert(kChecksumOffset + kChecksumSize == SERVER_IDENTITY_FILE_SIZE);
        static_assert(SERVER_SIGNING_SECRET_SIZE == crypto_sign_SECRETKEYBYTES);

        using IdentityBytes = std::array<uint8_t, SERVER_IDENTITY_FILE_SIZE>;

        void StoreLE32(uint8_t* p, uint32_t v)
        {
            for (size_t i = 0; i < 4; ++i)
            {
                p[i] = static_cast<uint8_t>(v >> (i * 8));
            }
        }

        uint32_t LoadLE32(const uint8_t* p)
        {
            return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
        }

        std::array<uint8_t, kChecksumSize> Checksum(const IdentityBytes& bytes)
        {
            std::array<uint8_t, crypto_hash_sha256_BYTES> digest{};
            crypto_hash_sha256(digest.data(), bytes.data(), kChecksumOffset);
            std::array<uint8_t, kChecksumSize> truncated{};
            std::copy_n(digest.begin(), kChecksumSize, truncated.begin());
            return truncated;
        }

        IdentityBytes EncodeIdentity(const ServerIdentity& identity)
        {
            IdentityBytes bytes{};
            std::copy(kIdentityMagic.begin(), kIdentityMagic.end(), bytes.begin() + kMagicOffset);
            StoreLE32(&bytes[kVersionOffset], SERVER_IDENTITY_FILE_VERSION);
            StoreLE32(&bytes[kReservedOffset], 0);
            std::copy(identity.secretKey.begin(), identity.secretKey.end(), bytes.begin() + kSecretOffset);
            const auto checksum = Checksum(bytes);
            std::copy(checksum.begin(), checksum.end(), bytes.begin() + kChecksumOffset);
            return bytes;
        }

        std::expected<ServerIdentity, TrustStoreError> DecodeIdentity(const IdentityBytes& bytes)
        {
            if (!std::equal(kIdentityMagic.begin(), kIdentityMagic.end(), bytes.begin() + kMagicOffset) ||
                LoadLE32(&bytes[kVersionOffset]) != SERVER_IDENTITY_FILE_VERSION ||
                LoadLE32(&bytes[kReservedOffset]) != 0)
            {
                return std::unexpected(TrustStoreError::Corrupt);
            }
            const auto checksum = Checksum(bytes);
            if (!std::equal(checksum.begin(), checksum.end(), bytes.begin() + kChecksumOffset))
            {
                return std::unexpected(TrustStoreError::Corrupt);
            }

            // The Ed25519 secret key is seed || public key. Re-derive the pair from
            // the seed so a file whose public half was altered (checksum and all) is
            // refused instead of signing under a key clients never pinned.
            ServerIdentity identity;
            std::array<uint8_t, crypto_sign_SEEDBYTES> seed{};
            std::copy_n(bytes.begin() + kSecretOffset, seed.size(), seed.begin());
            crypto_sign_seed_keypair(identity.publicKey.data(), identity.secretKey.data(), seed.data());
            sodium_memzero(seed.data(), seed.size());
            if (sodium_memcmp(identity.secretKey.data(), &bytes[kSecretOffset], identity.secretKey.size()) != 0)
            {
                return std::unexpected(TrustStoreError::Corrupt);
            }
            return identity;
        }

        /// Regular-file check without following a final symlink.
        std::expected<void, TrustStoreError> CheckRegularFile(const std::filesystem::path& path)
        {
            std::error_code error;
            const auto status = std::filesystem::symlink_status(path, error);
            if (error || status.type() == std::filesystem::file_type::not_found)
            {
                return std::unexpected(status.type() == std::filesystem::file_type::not_found
                                           ? TrustStoreError::NotFound
                                           : TrustStoreError::IoError);
            }
            if (status.type() != std::filesystem::file_type::regular)
            {
                return std::unexpected(TrustStoreError::NotRegularFile);
            }
            return {};
        }

        std::expected<std::string, TrustStoreError> ReadBounded(const std::filesystem::path& path, size_t maxBytes)
        {
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            if (error)
            {
                return std::unexpected(TrustStoreError::IoError);
            }
            if (size > maxBytes)
            {
                return std::unexpected(TrustStoreError::TooLarge);
            }
            std::ifstream stream(path, std::ios::binary);
            if (!stream)
            {
                return std::unexpected(TrustStoreError::IoError);
            }
            std::string contents(static_cast<size_t>(size), '\0');
            stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
            if (stream.gcount() != static_cast<std::streamsize>(contents.size()))
            {
                Spark::SecureClear(contents);
                return std::unexpected(TrustStoreError::IoError);
            }
            // A file that grew between the size query and the read is refused.
            if (stream.peek() != std::char_traits<char>::eof())
            {
                Spark::SecureClear(contents);
                return std::unexpected(TrustStoreError::TooLarge);
            }
            return contents;
        }

        /// Owner-only temporary sibling of @p target that does not exist yet.
        std::filesystem::path TemporarySibling(const std::filesystem::path& target)
        {
            std::filesystem::path temporary = target;
            temporary += "." + Spark::SecureRandom::HexToken(8) + ".tmp";
            return temporary;
        }

        bool IsLowerHex(char c)
        {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        }

        uint8_t HexNibble(char c)
        {
            return static_cast<uint8_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        }

        /// "<dotted ipv4>:<port>" in exactly the form FormatKnownHostsEndpoint writes.
        bool IsCanonicalEndpoint(std::string_view endpoint)
        {
            const size_t colon = endpoint.rfind(':');
            if (colon == std::string_view::npos || colon == 0 || colon + 1 >= endpoint.size())
            {
                return false;
            }
            uint32_t address = 0;
            if (!ParseIPv4Address(endpoint.substr(0, colon), address))
            {
                return false;
            }
            unsigned port = 0;
            const std::string_view portText = endpoint.substr(colon + 1);
            const auto [end, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
            if (ec != std::errc() || end != portText.data() + portText.size() || port == 0 || port > 65535)
            {
                return false;
            }
            return FormatKnownHostsEndpoint(address, static_cast<uint16_t>(port)) == endpoint;
        }

        std::string SerializeKnownHosts(const KnownHosts& hosts)
        {
            static constexpr char kHex[] = "0123456789abcdef";
            std::string text;
            for (const auto& [endpoint, key] : hosts)
            {
                text += endpoint;
                text += ' ';
                for (const uint8_t byte : key)
                {
                    text += kHex[byte >> 4];
                    text += kHex[byte & 0x0F];
                }
                text += '\n';
            }
            return text;
        }

        bool IsAllZero(const ServerPublicKey& key)
        {
            return std::all_of(key.begin(), key.end(), [](uint8_t b) { return b == 0; });
        }
    } // namespace

    const char* TrustStoreErrorText(TrustStoreError error) noexcept
    {
        switch (error)
        {
        case TrustStoreError::NotFound:
            return "file not found";
        case TrustStoreError::NotRegularFile:
            return "not a regular file";
        case TrustStoreError::WrongSize:
            return "identity file has the wrong size";
        case TrustStoreError::Corrupt:
            return "identity file is corrupt";
        case TrustStoreError::InsecurePermissions:
            return "identity file is accessible to other users";
        case TrustStoreError::IoError:
            return "file I/O failed";
        case TrustStoreError::CsprngFailure:
            return "key generation failed";
        case TrustStoreError::Malformed:
            return "known_hosts is malformed";
        case TrustStoreError::TooLarge:
            return "known_hosts is too large";
        case TrustStoreError::TooManyEntries:
            return "known_hosts has too many entries";
        case TrustStoreError::InvalidEndpoint:
            return "endpoint is not a canonical ipv4:port";
        case TrustStoreError::ServerIdentityMismatch:
            return "server key differs from the recorded key";
        case TrustStoreError::NotConfigured:
            return "server trust is not configured";
        }
        return "unknown trust-store error";
    }

    // ============================================================================
    // Server identity
    // ============================================================================

    std::expected<ServerIdentity, TrustStoreError> LoadServerIdentity(const std::filesystem::path& path)
    {
        if (auto regular = CheckRegularFile(path); !regular)
        {
            return std::unexpected(regular.error());
        }
        std::error_code error;
        const auto size = std::filesystem::file_size(path, error);
        if (error)
        {
            return std::unexpected(TrustStoreError::IoError);
        }
        if (size != SERVER_IDENTITY_FILE_SIZE)
        {
            return std::unexpected(TrustStoreError::WrongSize);
        }
#ifndef SPARK_PLATFORM_WINDOWS
        // Windows ACLs are applied at creation (SecureRandom::CreatePrivateFile);
        // on POSIX the mode bits are the whole story, so check them on every load.
        const auto permissions = std::filesystem::status(path, error).permissions();
        if (error)
        {
            return std::unexpected(TrustStoreError::IoError);
        }
        using Perms = std::filesystem::perms;
        if ((permissions & (Perms::group_all | Perms::others_all)) != Perms::none)
        {
            return std::unexpected(TrustStoreError::InsecurePermissions);
        }
#endif
        if (!EnsureSodium())
        {
            return std::unexpected(TrustStoreError::CsprngFailure);
        }

        auto contents = ReadBounded(path, SERVER_IDENTITY_FILE_SIZE);
        if (!contents)
        {
            return std::unexpected(contents.error() == TrustStoreError::TooLarge ? TrustStoreError::WrongSize
                                                                                 : contents.error());
        }
        if (contents->size() != SERVER_IDENTITY_FILE_SIZE)
        {
            Spark::SecureClear(*contents);
            return std::unexpected(TrustStoreError::WrongSize);
        }
        IdentityBytes bytes{};
        std::memcpy(bytes.data(), contents->data(), bytes.size());
        Spark::SecureClear(*contents);
        auto identity = DecodeIdentity(bytes);
        sodium_memzero(bytes.data(), bytes.size());
        return identity;
    }

    std::expected<ServerIdentity, TrustStoreError> LoadOrCreateServerIdentity(const std::filesystem::path& path)
    {
        auto existing = LoadServerIdentity(path);
        if (existing || existing.error() != TrustStoreError::NotFound)
        {
            return existing;
        }

        auto generated = GenerateServerIdentity();
        if (!generated)
        {
            return std::unexpected(TrustStoreError::CsprngFailure);
        }

        std::error_code error;
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return std::unexpected(TrustStoreError::IoError);
            }
        }

        // Write the whole file owner-only under a temporary name, then publish it
        // with a hard link, which fails if the target exists. A crash leaves at
        // most a stray temporary, and a concurrent creator's identity is kept.
        IdentityBytes bytes = EncodeIdentity(*generated);
        const std::filesystem::path temporary = TemporarySibling(path);
        std::string createError;
        const bool created = Spark::SecureRandom::CreatePrivateFile(
            temporary, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), &createError);
        sodium_memzero(bytes.data(), bytes.size());
        if (!created)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Cannot create server identity '%s': %s",
                            temporary.string().c_str(), createError.c_str());
            return std::unexpected(TrustStoreError::IoError);
        }
        std::filesystem::create_hard_link(temporary, path, error);
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        if (error)
        {
            // Lost a creation race (or the volume has no hard links): whatever is
            // at the path now is the identity, and it must load on its own merits.
            return LoadServerIdentity(path);
        }
        SPARK_LOG_INFO(Spark::LogCategory::Network, "Created server identity '%s'", path.string().c_str());
        return LoadServerIdentity(path);
    }

    // ============================================================================
    // Client trust
    // ============================================================================

    ServerTrust ServerTrust::Pin(const ServerPublicKey& key)
    {
        ServerTrust trust;
        trust.mode = Mode::Pinned;
        trust.pinnedKey = key;
        return trust;
    }

    ServerTrust ServerTrust::TrustOnFirstUseAt(std::filesystem::path knownHosts)
    {
        ServerTrust trust;
        trust.mode = Mode::TrustOnFirstUse;
        trust.knownHostsPath = std::move(knownHosts);
        return trust;
    }

    bool ServerTrust::IsUsable() const noexcept
    {
        if (mode == Mode::Pinned)
        {
            return !IsAllZero(pinnedKey);
        }
        return !knownHostsPath.empty();
    }

    std::string FormatKnownHostsEndpoint(uint32_t ipv4HostOrder, uint16_t port)
    {
        return FormatIPv4Address(ipv4HostOrder) + ":" + std::to_string(port);
    }

    std::expected<KnownHosts, TrustStoreError> ParseKnownHosts(std::string_view text)
    {
        if (text.size() > KNOWN_HOSTS_MAX_BYTES)
        {
            return std::unexpected(TrustStoreError::TooLarge);
        }
        constexpr size_t kHexKeyLength = HANDSHAKE_PUBLIC_KEY_SIZE * 2;

        KnownHosts hosts;
        size_t lineStart = 0;
        while (lineStart < text.size())
        {
            const size_t lineEnd = text.find('\n', lineStart);
            if (lineEnd == std::string_view::npos)
            {
                // Every line, including the last, ends with '\n' (as written by SerializeKnownHosts).
                return std::unexpected(TrustStoreError::Malformed);
            }
            const std::string_view line = text.substr(lineStart, lineEnd - lineStart);
            lineStart = lineEnd + 1;

            const size_t space = line.find(' ');
            if (space == std::string_view::npos || line.size() != space + 1 + kHexKeyLength)
            {
                return std::unexpected(TrustStoreError::Malformed);
            }
            const std::string_view endpoint = line.substr(0, space);
            const std::string_view hex = line.substr(space + 1);
            if (!IsCanonicalEndpoint(endpoint) || !std::all_of(hex.begin(), hex.end(), IsLowerHex))
            {
                return std::unexpected(TrustStoreError::Malformed);
            }
            ServerPublicKey key{};
            for (size_t i = 0; i < key.size(); ++i)
            {
                key[i] = static_cast<uint8_t>((HexNibble(hex[2 * i]) << 4) | HexNibble(hex[2 * i + 1]));
            }
            if (hosts.size() >= KNOWN_HOSTS_MAX_ENTRIES)
            {
                return std::unexpected(TrustStoreError::TooManyEntries);
            }
            if (!hosts.emplace(std::string(endpoint), key).second)
            {
                return std::unexpected(TrustStoreError::Malformed); // duplicate endpoint
            }
        }
        return hosts;
    }

    std::expected<KnownHosts, TrustStoreError> LoadKnownHosts(const std::filesystem::path& path)
    {
        if (auto regular = CheckRegularFile(path); !regular)
        {
            if (regular.error() == TrustStoreError::NotFound)
            {
                return KnownHosts{};
            }
            return std::unexpected(regular.error());
        }
        auto contents = ReadBounded(path, KNOWN_HOSTS_MAX_BYTES);
        if (!contents)
        {
            return std::unexpected(contents.error());
        }
        return ParseKnownHosts(*contents);
    }

    std::expected<void, TrustStoreError> RecordKnownHost(const std::filesystem::path& path, const std::string& endpoint,
                                                         const ServerPublicKey& key)
    {
        if (!IsCanonicalEndpoint(endpoint))
        {
            return std::unexpected(TrustStoreError::InvalidEndpoint);
        }
        auto hosts = LoadKnownHosts(path);
        if (!hosts)
        {
            return std::unexpected(hosts.error());
        }
        if (const auto it = hosts->find(endpoint); it != hosts->end())
        {
            if (sodium_memcmp(it->second.data(), key.data(), key.size()) != 0)
            {
                return std::unexpected(TrustStoreError::ServerIdentityMismatch);
            }
            return {};
        }
        if (hosts->size() >= KNOWN_HOSTS_MAX_ENTRIES)
        {
            return std::unexpected(TrustStoreError::TooManyEntries);
        }
        hosts->emplace(endpoint, key);
        const std::string text = SerializeKnownHosts(*hosts);
        if (text.size() > KNOWN_HOSTS_MAX_BYTES)
        {
            return std::unexpected(TrustStoreError::TooLarge);
        }

        std::error_code error;
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), error);
            if (error)
            {
                return std::unexpected(TrustStoreError::IoError);
            }
        }
        // Whole-file replace through an owner-only temporary: a reader sees either
        // the old store or the new one, never a torn line.
        const std::filesystem::path temporary = TemporarySibling(path);
        std::string createError;
        if (!Spark::SecureRandom::CreatePrivateFile(temporary, text, &createError))
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Network, "Cannot write known_hosts '%s': %s",
                            temporary.string().c_str(), createError.c_str());
            return std::unexpected(TrustStoreError::IoError);
        }
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            std::error_code removeError;
            std::filesystem::remove(temporary, removeError);
            return std::unexpected(TrustStoreError::IoError);
        }
        return {};
    }

    std::expected<ExpectedServerKey, TrustStoreError> ResolveExpectedServerKey(const ServerTrust& trust,
                                                                               const std::string& endpoint,
                                                                               const ServerPublicKey& presented)
    {
        if (!trust.IsUsable())
        {
            return std::unexpected(TrustStoreError::NotConfigured);
        }
        if (trust.mode == ServerTrust::Mode::Pinned)
        {
            return ExpectedServerKey{trust.pinnedKey, false};
        }
        if (!IsCanonicalEndpoint(endpoint))
        {
            return std::unexpected(TrustStoreError::InvalidEndpoint);
        }
        auto hosts = LoadKnownHosts(trust.knownHostsPath);
        if (!hosts)
        {
            return std::unexpected(hosts.error());
        }
        if (const auto it = hosts->find(endpoint); it != hosts->end())
        {
            return ExpectedServerKey{it->second, false};
        }
        return ExpectedServerKey{presented, true};
    }

    std::filesystem::path DefaultNetworkSecurityDirectory()
    {
        const std::filesystem::path userData = Spark::UserPaths::GetUserDataDir();
        return userData.empty() ? userData : userData / "net";
    }

} // namespace Spark::Net
