/**
 * @file FuzzOrchestratorIdentityProduction.cpp
 * @brief libc++-compiled production adapter for the SparkOrchestrator identity harness.
 *
 * Every SparkOrchestrator mutation (define/start/stop/drain/restart) first takes an
 * OrchestratorIdentityLease: Acquire locks <state>.lock, reads the persisted
 * "SPORCHCLI1" state and republishes it with the next sequence, which the daemon's
 * exactly-once replay table keys on. The input is written as a private (0600) state
 * file. A violated contract aborts so libFuzzer records a crash:
 *  - Acquire accepts exactly what an independent model accepts: an empty file (a
 *    fresh identity), or the magic line, a 1..64-byte client-instance line and a
 *    plain decimal last sequence below UINT64_MAX ending in one newline,
 *  - an accepted state yields that client instance and last sequence + 1 (a fresh
 *    identity starts at sequence 1), and the file then holds exactly that state,
 *  - a rejected state reports an error and leaves the file byte-for-byte intact,
 *  - a second Acquire continues the same client at the next sequence (or refuses an
 *    exhausted sequence), so no two mutations ever share a key.
 */

#include "FuzzOrchestratorIdentityProduction.h"

#include "OrchestratorIdentity.h"

#include <atomic>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace
{
    namespace fs = std::filesystem;
    namespace Daemon = Spark::Daemon;

    constexpr std::size_t kMaxInputBytes = 257;
    constexpr std::string_view kMagic = "SPORCHCLI1\n";
    std::atomic<std::uint64_t> s_directoryCounter{0};

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzOrchestratorIdentity: %s\n", what);
        std::abort();
    }

    /// A fresh directory for one input; removed with its contents on scope exit.
    class ScopedDirectory
    {
      public:
        ScopedDirectory()
        {
            const auto suffix = s_directoryCounter.fetch_add(1, std::memory_order_relaxed);
            m_path = fs::temp_directory_path() /
                     ("spark-fuzz-orchestrator-identity-" + std::to_string(getpid()) + "-" + std::to_string(suffix));
            std::error_code error;
            fs::remove_all(m_path, error);
            m_created = fs::create_directory(m_path, error) && !error;
        }
        ~ScopedDirectory()
        {
            std::error_code ignored;
            fs::remove_all(m_path, ignored);
        }
        ScopedDirectory(const ScopedDirectory&) = delete;
        ScopedDirectory& operator=(const ScopedDirectory&) = delete;

        bool Created() const { return m_created; }
        const fs::path& Path() const { return m_path; }

      private:
        fs::path m_path;
        bool m_created = false;
    };

    std::string ReadAll(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }

    struct ModelKey
    {
        std::string clientInstance;
        std::uint64_t lastSequence = 0;
    };

    /// Independent model of the state format. nullopt: rejected; a key with an empty client: fresh identity.
    std::optional<ModelKey> ModelDecode(std::string_view bytes)
    {
        if (bytes.empty())
        {
            return ModelKey{};
        }
        if (bytes.size() > 256 || !bytes.starts_with(kMagic))
        {
            return std::nullopt;
        }
        bytes.remove_prefix(kMagic.size());
        const std::size_t newline = bytes.find('\n');
        if (newline == std::string_view::npos || newline == 0 || newline > Daemon::kMaximumClientInstanceLength)
        {
            return std::nullopt;
        }
        ModelKey key{std::string(bytes.substr(0, newline)), 0};
        std::string_view digits = bytes.substr(newline + 1);
        if (digits.size() < 2 || digits.back() != '\n')
        {
            return std::nullopt;
        }
        digits.remove_suffix(1);
        for (const char c : digits)
        {
            if (c < '0' || c > '9')
            {
                return std::nullopt;
            }
        }
        const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), key.lastSequence);
        if (error != std::errc{} || end != digits.data() + digits.size() ||
            key.lastSequence == std::numeric_limits<std::uint64_t>::max())
        {
            return std::nullopt;
        }
        return key;
    }

    std::string Encoded(const Daemon::MutationKey& key)
    {
        return std::string(kMagic) + key.clientInstance + "\n" + std::to_string(key.sequence) + "\n";
    }
} // namespace

extern "C" int SparkFuzzAcquireOrchestratorIdentity(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string input(reinterpret_cast<const char*>(data), size);
    ScopedDirectory directory;
    if (!directory.Created())
    {
        return 0;
    }
    const fs::path state = directory.Path() / "orchestrator.identity";
    {
        std::ofstream file(state, std::ios::binary | std::ios::trunc);
        file.write(input.data(), static_cast<std::streamsize>(input.size()));
        if (!file)
        {
            return 0;
        }
    }
    std::error_code permissionError;
    fs::permissions(state, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, permissionError);
    if (permissionError)
    {
        return 0;
    }

    const std::optional<ModelKey> expected = ModelDecode(input);
    Daemon::MutationKey key;
    {
        std::string error;
        const auto lease = Daemon::OrchestratorIdentityLease::Acquire(state, error);
        if (lease.has_value() != expected.has_value())
        {
            InvariantFailure(lease ? "Acquire accepted a state the format model rejects"
                                   : "Acquire rejected a state the format model accepts");
        }
        if (!lease)
        {
            if (error.empty())
            {
                InvariantFailure("a rejected state reported no error");
            }
            if (ReadAll(state) != input)
            {
                InvariantFailure("a rejected state file was modified");
            }
            return 0;
        }
        key = lease->Key();
        if (key.clientInstance.empty() || key.clientInstance.size() > Daemon::kMaximumClientInstanceLength ||
            key.clientInstance.find('\n') != std::string::npos)
        {
            InvariantFailure("the lease holds a client instance the state file cannot carry");
        }
        const bool fresh = expected->clientInstance.empty();
        if (fresh ? key.sequence != 1
                  : key.clientInstance != expected->clientInstance || key.sequence != expected->lastSequence + 1)
        {
            InvariantFailure("the lease is not the persisted client at its next sequence");
        }
        if (ReadAll(state) != Encoded(key))
        {
            InvariantFailure("the republished state does not hold the leased key");
        }
    }

    std::string error;
    const auto next = Daemon::OrchestratorIdentityLease::Acquire(state, error);
    if (key.sequence == std::numeric_limits<std::uint64_t>::max())
    {
        if (next)
        {
            InvariantFailure("an exhausted sequence was leased again");
        }
        return 0;
    }
    if (!next || next->Key().clientInstance != key.clientInstance || next->Key().sequence != key.sequence + 1)
    {
        InvariantFailure("a second lease does not continue the same client at the next sequence");
    }
    return 0;
}
