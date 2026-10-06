/**
 * @file NetworkTrustStore.h
 * @brief NET-100 server identity persistence and client server-trust (pinning, trust on first use)
 * @author Spark Engine Team
 * @date 2026
 *
 * The transport has no unauthenticated mode: a server needs a long-term Ed25519
 * identity (SecureHandshake.h) and a client needs to know which server key to
 * accept. This file provides both halves.
 *
 * Server identity file (exactly SERVER_IDENTITY_FILE_SIZE = 96 bytes):
 *   [magic 8 = "SPKSVID\0"][version u32 LE = 1][reserved u32 = 0]
 *   [Ed25519 secret key 64 (seed || public key)][checksum 16 = SHA-256(bytes 0..80)[0..16]]
 * LoadOrCreateServerIdentity creates it owner-only (0600 on POSIX, a protected
 * owner-only DACL on Windows, via SecureRandom::CreatePrivateFile) and publishes
 * it with an exclusive hard link, so a concurrent creator never overwrites it.
 * The loader rejects anything that is not a regular file of exactly 96 bytes,
 * a wrong magic/version/reserved field, a bad checksum, a secret key whose
 * public half does not match its seed and, on POSIX, a file readable by group
 * or others.
 *
 * Client trust (ServerTrust):
 *  - Pinned: the client accepts exactly one server key. An all-zero key is a
 *    configuration error and NetworkManager::Connect refuses it.
 *  - TrustOnFirstUse: a known_hosts file maps "<ipv4>:<port>" to a key. The
 *    first successful, signature-verified handshake with an unknown endpoint
 *    records the key; any later mismatch fails with ServerIdentityMismatch and
 *    the stored key is never replaced silently (delete the line to re-trust).
 * known_hosts is text, one "<dotted ipv4>:<port> <64 lowercase hex>" per line,
 * at most KNOWN_HOSTS_MAX_ENTRIES lines and KNOWN_HOSTS_MAX_BYTES bytes. Parsing
 * is strict: any malformed or duplicate line fails the whole file closed.
 *
 * Thread affinity: none; every function is reentrant. Callers serialize access
 * to one known_hosts file (NetworkManager does so under its API lock).
 * Ownership: returned ServerIdentity values own their secret and wipe it.
 * Allocation: file I/O and small containers at connection setup only.
 * Scalability tier: connection setup, never per packet.
 */

#pragma once

#include "ConnectRateLimiter.h"
#include "SecureHandshake.h"

#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace Spark::Net
{

    constexpr size_t SERVER_IDENTITY_FILE_SIZE = 96;
    constexpr uint32_t SERVER_IDENTITY_FILE_VERSION = 1;
    constexpr size_t KNOWN_HOSTS_MAX_ENTRIES = 4096;
    constexpr size_t KNOWN_HOSTS_MAX_BYTES = size_t{1024} * 1024;

    /** @brief Why a trust-store operation failed. Every value means "do not trust". */
    enum class TrustStoreError : uint8_t
    {
        NotFound,               ///< The file does not exist
        NotRegularFile,         ///< A directory, symlink, device or other non-regular file
        WrongSize,              ///< Identity file is not exactly SERVER_IDENTITY_FILE_SIZE bytes
        Corrupt,                ///< Bad magic, version, reserved field, checksum or key pair
        InsecurePermissions,    ///< POSIX: group or others may access the identity file
        IoError,                ///< Read, write, create or rename failed
        CsprngFailure,          ///< libsodium could not generate a key
        Malformed,              ///< A known_hosts line is not "<ipv4>:<port> <hex64>" or is duplicated
        TooLarge,               ///< known_hosts exceeds KNOWN_HOSTS_MAX_BYTES
        TooManyEntries,         ///< known_hosts would exceed KNOWN_HOSTS_MAX_ENTRIES
        InvalidEndpoint,        ///< Endpoint text is not a canonical "<ipv4>:<port>"
        ServerIdentityMismatch, ///< The endpoint is recorded with a different key
        NotConfigured           ///< ServerTrust is not usable (zero pin, or TOFU without a path)
    };

    /** @brief Stable human-readable text for a TrustStoreError. */
    [[nodiscard]] const char* TrustStoreErrorText(TrustStoreError error) noexcept;

    /**
     * @brief Load a server identity file (see the file comment for the format)
     * @param path Identity file
     * @return The identity, or why the file was refused
     */
    [[nodiscard]] std::expected<ServerIdentity, TrustStoreError> LoadServerIdentity(const std::filesystem::path& path);

    /**
     * @brief Load the identity at @p path, creating a fresh owner-only one when the file does not exist
     *
     * An existing but invalid file is an error, never replaced: an operator must
     * look at it, because clients have pinned the key it held.
     *
     * @param path Identity file; missing parent directories are created
     * @return The identity, or why it could not be loaded or created
     */
    [[nodiscard]] std::expected<ServerIdentity, TrustStoreError> LoadOrCreateServerIdentity(
        const std::filesystem::path& path);

    /** @brief Which server keys a client accepts. */
    struct ServerTrust
    {
        enum class Mode : uint8_t
        {
            Pinned,         ///< Accept exactly pinnedKey
            TrustOnFirstUse ///< Record the first verified key per endpoint in knownHostsPath
        };

        Mode mode = Mode::Pinned;
        ServerPublicKey pinnedKey{};          ///< Pinned mode only; all zero means "not configured"
        std::filesystem::path knownHostsPath; ///< TrustOnFirstUse only; must be non-empty

        /** @brief Trust exactly @p key. */
        [[nodiscard]] static ServerTrust Pin(const ServerPublicKey& key);
        /** @brief Trust on first use, persisted in @p knownHosts. */
        [[nodiscard]] static ServerTrust TrustOnFirstUseAt(std::filesystem::path knownHosts);

        /** @brief False for a zero pin or a TOFU trust without a path; Connect refuses those. */
        [[nodiscard]] bool IsUsable() const noexcept;
    };

    /** @brief known_hosts contents, keyed by canonical "<ipv4>:<port>". */
    using KnownHosts = std::map<std::string, ServerPublicKey>;

    /** @brief Canonical known_hosts endpoint text for a host-order IPv4 address and port. */
    [[nodiscard]] std::string FormatKnownHostsEndpoint(uint32_t ipv4HostOrder, uint16_t port);

    /**
     * @brief Parse known_hosts text strictly
     * @param text File contents (at most KNOWN_HOSTS_MAX_BYTES)
     * @return The entries, or Malformed / TooLarge / TooManyEntries
     */
    [[nodiscard]] std::expected<KnownHosts, TrustStoreError> ParseKnownHosts(std::string_view text);

    /**
     * @brief Load known_hosts; a missing file is an empty store
     * @param path known_hosts file
     * @return The entries, or why the file was refused (never a partial store)
     */
    [[nodiscard]] std::expected<KnownHosts, TrustStoreError> LoadKnownHosts(const std::filesystem::path& path);

    /**
     * @brief Record @p key for @p endpoint, atomically rewriting known_hosts
     *
     * Recording the key already stored is a no-op. A different stored key returns
     * ServerIdentityMismatch and leaves the file byte-for-byte unchanged.
     *
     * @param path     known_hosts file; missing parent directories are created
     * @param endpoint Canonical "<ipv4>:<port>" (FormatKnownHostsEndpoint)
     * @param key      Server key that just completed a verified handshake
     * @return Nothing on success, or why nothing was recorded
     */
    [[nodiscard]] std::expected<void, TrustStoreError> RecordKnownHost(const std::filesystem::path& path,
                                                                       const std::string& endpoint,
                                                                       const ServerPublicKey& key);

    /** @brief The key a client must require from a server, and whether it comes from first use. */
    struct ExpectedServerKey
    {
        ServerPublicKey key{};
        bool firstUse = false; ///< TOFU: nothing was recorded; record key after the handshake verifies
    };

    /**
     * @brief Decide which key the server at @p endpoint must prove it holds
     * @param trust     Client trust configuration
     * @param endpoint  Canonical "<ipv4>:<port>" of the server
     * @param presented Key the server named in its ServerHello (used only on TOFU first use)
     * @return The key to hand to ClientHandshake::Finish, or why the client must refuse
     */
    [[nodiscard]] std::expected<ExpectedServerKey, TrustStoreError> ResolveExpectedServerKey(
        const ServerTrust& trust, const std::string& endpoint, const ServerPublicKey& presented);

    /**
     * @brief Transport security configuration held by NetworkManager
     *
     * A server needs @c identity; a client needs a usable @c trust. There is no
     * field that disables authentication or encryption.
     */
    struct NetworkSecurityConfig
    {
        std::optional<ServerIdentity> identity; ///< Server role: this server's signing identity
        ServerTrust trust;                      ///< Client role: which server keys to accept
        ConnectRateLimit connectRate;           ///< Server role: unadmitted Connects per source IPv4
    };

    /**
     * @brief Default directory for the identity file and known_hosts: <user data>/net
     * @return Empty when the platform has no per-user data directory
     */
    [[nodiscard]] std::filesystem::path DefaultNetworkSecurityDirectory();

    constexpr const char* DEFAULT_SERVER_IDENTITY_FILE = "server_identity.key";
    constexpr const char* DEFAULT_KNOWN_HOSTS_FILE = "known_hosts";

} // namespace Spark::Net
