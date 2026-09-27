/**
 * @file TestNET100TrustStore.cpp
 * @brief NET-100: server identity file, known_hosts trust-on-first-use, and the no-identity refusal.
 *
 * Every case drives the shipped NetworkTrustStore / NetworkManager code against real files
 * in a private temporary directory. Anything that is not exactly a valid identity file or a
 * strictly well-formed known_hosts must fail closed and leave the stored trust unchanged.
 */

#include "TestFramework.h"
#include "Fixtures/NetworkTestSecurity.h"
#include "Engine/Networking/NetworkManager.h"
#include "Engine/Networking/NetworkTrustStore.h"
#include "Utils/SecureRandom.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace Spark::Net;

namespace
{
    /// Private scratch directory removed when the test ends.
    class TempDir
    {
      public:
        TempDir()
            : m_path(std::filesystem::temp_directory_path() /
                     ("spark-net100-trust-" + Spark::SecureRandom::HexToken(8)))
        {
            std::filesystem::create_directories(m_path);
        }
        ~TempDir()
        {
            std::error_code error;
            std::filesystem::remove_all(m_path, error);
        }
        TempDir(const TempDir&) = delete;
        TempDir& operator=(const TempDir&) = delete;

        const std::filesystem::path& Path() const { return m_path; }

      private:
        std::filesystem::path m_path;
    };

    std::vector<uint8_t> ReadAll(const std::filesystem::path& path)
    {
        std::ifstream stream(path, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }

    std::string ReadText(const std::filesystem::path& path)
    {
        const auto bytes = ReadAll(path);
        return std::string(bytes.begin(), bytes.end());
    }

    void WriteAll(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
    {
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
                                     std::filesystem::perm_options::replace);
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void WriteText(const std::filesystem::path& path, const std::string& text)
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << text;
    }

    ServerPublicKey KeyFilledWith(uint8_t value)
    {
        ServerPublicKey key{};
        key.fill(value);
        return key;
    }
} // namespace

TEST(Transport_TrustStore_IdentityRoundTrip)
{
    TempDir dir;
    const auto path = dir.Path() / "nested" / "server_identity.key";

    auto created = LoadOrCreateServerIdentity(path);
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(std::filesystem::file_size(path), static_cast<std::uintmax_t>(SERVER_IDENTITY_FILE_SIZE));
    EXPECT_TRUE(std::filesystem::is_regular_file(path));

    // No stray temporaries are left beside the published file.
    size_t entries = 0;
    for ([[maybe_unused]] const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
        ++entries;
    EXPECT_EQ(entries, static_cast<size_t>(1));

#ifndef _WIN32
    const auto perms = std::filesystem::status(path).permissions();
    EXPECT_TRUE((perms & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) ==
                std::filesystem::perms::none);
#endif

    // A second start loads the same key instead of minting a new one.
    auto reloaded = LoadOrCreateServerIdentity(path);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_TRUE(reloaded->publicKey == created->publicKey);
    EXPECT_TRUE(reloaded->secretKey == created->secretKey);

    // The loaded identity signs handshakes a client pinned to its key accepts.
    ClientHandshake client;
    auto hello = client.Begin(NETWORK_PROTOCOL_VERSION);
    ASSERT_TRUE(hello.has_value());
    auto response = RespondToClientHello(*hello, *reloaded);
    ASSERT_TRUE(response.has_value());
    EXPECT_TRUE(client.Finish(response->serverHello, created->publicKey).has_value());
}

TEST(Transport_TrustStore_CorruptIdentityRejected)
{
    TempDir dir;
    const auto path = dir.Path() / "server_identity.key";
    ASSERT_TRUE(LoadOrCreateServerIdentity(path).has_value());
    const auto good = ReadAll(path);
    ASSERT_EQ(good.size(), SERVER_IDENTITY_FILE_SIZE);

    // One flipped bit anywhere (magic, version, secret, checksum) is refused.
    for (const size_t offset : {size_t{0}, size_t{8}, size_t{12}, size_t{20}, size_t{60}, size_t{90}})
    {
        auto flipped = good;
        flipped[offset] ^= 0x01;
        WriteAll(path, flipped);
        auto loaded = LoadServerIdentity(path);
        EXPECT_FALSE(loaded.has_value());
        if (!loaded)
            EXPECT_EQ(static_cast<int>(loaded.error()), static_cast<int>(TrustStoreError::Corrupt));
    }

    // Short and oversized files are refused before any parsing.
    WriteAll(path, std::vector<uint8_t>(good.begin(), good.end() - 1));
    auto shortFile = LoadServerIdentity(path);
    ASSERT_FALSE(shortFile.has_value());
    EXPECT_EQ(static_cast<int>(shortFile.error()), static_cast<int>(TrustStoreError::WrongSize));

    auto longBytes = good;
    longBytes.push_back(0);
    WriteAll(path, longBytes);
    auto longFile = LoadServerIdentity(path);
    ASSERT_FALSE(longFile.has_value());
    EXPECT_EQ(static_cast<int>(longFile.error()), static_cast<int>(TrustStoreError::WrongSize));

    // LoadOrCreate never "repairs" an existing bad file: clients pinned the key it held.
    auto recreated = LoadOrCreateServerIdentity(path);
    EXPECT_FALSE(recreated.has_value());
    EXPECT_TRUE(ReadAll(path) == longBytes);

    // A directory where the identity should be is not a regular file.
    const auto directoryPath = dir.Path() / "identity-dir";
    std::filesystem::create_directories(directoryPath);
    auto directory = LoadServerIdentity(directoryPath);
    ASSERT_FALSE(directory.has_value());
    EXPECT_EQ(static_cast<int>(directory.error()), static_cast<int>(TrustStoreError::NotRegularFile));

    auto missing = LoadServerIdentity(dir.Path() / "absent.key");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(static_cast<int>(missing.error()), static_cast<int>(TrustStoreError::NotFound));

#ifndef _WIN32
    // A world- or group-readable identity is refused on POSIX.
    WriteAll(path, good);
    std::filesystem::permissions(path, std::filesystem::perms::group_read, std::filesystem::perm_options::add);
    auto exposed = LoadServerIdentity(path);
    ASSERT_FALSE(exposed.has_value());
    EXPECT_EQ(static_cast<int>(exposed.error()), static_cast<int>(TrustStoreError::InsecurePermissions));
#endif
}

TEST(Transport_TrustStore_TofuRecordsFirstUse)
{
    TempDir dir;
    const auto knownHosts = dir.Path() / "net" / "known_hosts";
    const ServerTrust trust = ServerTrust::TrustOnFirstUseAt(knownHosts);
    ASSERT_TRUE(trust.IsUsable());
    const std::string endpoint = FormatKnownHostsEndpoint(0x7F000001u, 27015);
    EXPECT_EQ(endpoint, std::string("127.0.0.1:27015"));

    const ServerPublicKey first = KeyFilledWith(0xA1);
    auto expected = ResolveExpectedServerKey(trust, endpoint, first);
    ASSERT_TRUE(expected.has_value());
    EXPECT_TRUE(expected->firstUse);
    EXPECT_TRUE(expected->key == first);
    EXPECT_FALSE(std::filesystem::exists(knownHosts)); // resolving records nothing

    ASSERT_TRUE(RecordKnownHost(knownHosts, endpoint, first).has_value());
    std::string expectedLine = endpoint + " ";
    for (int i = 0; i < 32; ++i)
        expectedLine += "a1";
    EXPECT_EQ(ReadText(knownHosts), expectedLine + "\n");

    // The recorded key is now required, whatever the server presents.
    auto again = ResolveExpectedServerKey(trust, endpoint, KeyFilledWith(0xB2));
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(again->firstUse);
    EXPECT_TRUE(again->key == first);

    // Re-recording the same key is idempotent; a second endpoint gets its own line.
    ASSERT_TRUE(RecordKnownHost(knownHosts, endpoint, first).has_value());
    ASSERT_TRUE(
        RecordKnownHost(knownHosts, FormatKnownHostsEndpoint(0x7F000001u, 27016), KeyFilledWith(0x0C)).has_value());
    auto hosts = LoadKnownHosts(knownHosts);
    ASSERT_TRUE(hosts.has_value());
    EXPECT_EQ(hosts->size(), static_cast<size_t>(2));

    // Pinned trust never consults known_hosts.
    auto pinned = ResolveExpectedServerKey(ServerTrust::Pin(KeyFilledWith(0x5A)), endpoint, first);
    ASSERT_TRUE(pinned.has_value());
    EXPECT_TRUE(pinned->key == KeyFilledWith(0x5A));
    EXPECT_FALSE(pinned->firstUse);
}

TEST(Transport_TrustStore_MismatchFailsAndLeavesStoreUnchanged)
{
    TempDir dir;
    const auto knownHosts = dir.Path() / "known_hosts";
    const std::string endpoint = FormatKnownHostsEndpoint(0x7F000001u, 4000);
    ASSERT_TRUE(RecordKnownHost(knownHosts, endpoint, KeyFilledWith(0x11)).has_value());
    const auto before = ReadAll(knownHosts);

    auto overwrite = RecordKnownHost(knownHosts, endpoint, KeyFilledWith(0x22));
    ASSERT_FALSE(overwrite.has_value());
    EXPECT_EQ(static_cast<int>(overwrite.error()), static_cast<int>(TrustStoreError::ServerIdentityMismatch));
    EXPECT_TRUE(ReadAll(knownHosts) == before);

    // A server presenting the other key is held to the recorded one (Finish then fails).
    auto expected = ResolveExpectedServerKey(ServerTrust::TrustOnFirstUseAt(knownHosts), endpoint, KeyFilledWith(0x22));
    ASSERT_TRUE(expected.has_value());
    EXPECT_FALSE(expected->firstUse);
    EXPECT_TRUE(expected->key == KeyFilledWith(0x11));

    // Non-canonical endpoints are refused rather than stored under a second spelling.
    auto spelling = RecordKnownHost(knownHosts, "127.000.000.001:4000", KeyFilledWith(0x22));
    ASSERT_FALSE(spelling.has_value());
    EXPECT_EQ(static_cast<int>(spelling.error()), static_cast<int>(TrustStoreError::InvalidEndpoint));
    EXPECT_TRUE(ReadAll(knownHosts) == before);
}

TEST(Transport_TrustStore_MalformedKnownHostsFailsClosed)
{
    const std::string key64(64, 'a');
    const std::vector<std::string> malformed = {
        "127.0.0.1:1 " + key64,                                                  // missing final newline
        "127.0.0.1:1 " + std::string(64, 'A') + "\n",                            // uppercase hex
        "127.0.0.1:1 " + std::string(62, 'a') + "\n",                            // short key
        "127.0.0.1:1  " + key64 + "\n",                                          // double space
        "127.0.0.1:0 " + key64 + "\n",                                           // port 0
        "127.0.0.1:65536 " + key64 + "\n",                                       // port overflow
        "127.0.0.01:1 " + key64 + "\n",                                          // non-canonical address
        "localhost:1 " + key64 + "\n",                                           // not an IPv4 literal
        "# comment\n",                                                           // no comment syntax
        "\n",                                                                    // blank line
        "127.0.0.1:1 " + key64 + "\n127.0.0.1:1 " + std::string(64, 'b') + "\n", // duplicate endpoint
    };
    for (const auto& text : malformed)
    {
        auto parsed = ParseKnownHosts(text);
        EXPECT_FALSE(parsed.has_value());
        if (!parsed)
            EXPECT_EQ(static_cast<int>(parsed.error()), static_cast<int>(TrustStoreError::Malformed));
    }
    EXPECT_TRUE(ParseKnownHosts("").has_value());
    EXPECT_TRUE(ParseKnownHosts("10.0.0.1:27015 " + key64 + "\n").has_value());

    // Entry and byte bounds.
    std::string tooMany;
    for (uint32_t i = 0; i <= KNOWN_HOSTS_MAX_ENTRIES; ++i)
        tooMany += FormatKnownHostsEndpoint(0x0A000000u + i, 1) + " " + key64 + "\n";
    auto many = ParseKnownHosts(tooMany);
    ASSERT_FALSE(many.has_value());
    EXPECT_EQ(static_cast<int>(many.error()), static_cast<int>(TrustStoreError::TooManyEntries));

    TempDir dir;
    const auto oversized = dir.Path() / "known_hosts";
    WriteText(oversized, std::string(KNOWN_HOSTS_MAX_BYTES + 1, 'x'));
    auto big = LoadKnownHosts(oversized);
    ASSERT_FALSE(big.has_value());
    EXPECT_EQ(static_cast<int>(big.error()), static_cast<int>(TrustStoreError::TooLarge));

    // A client with a malformed store refuses to trust anything and records nothing.
    const auto broken = dir.Path() / "broken_hosts";
    WriteText(broken, "garbage\n");
    auto resolved =
        ResolveExpectedServerKey(ServerTrust::TrustOnFirstUseAt(broken), "127.0.0.1:1", KeyFilledWith(0x33));
    ASSERT_FALSE(resolved.has_value());
    EXPECT_EQ(static_cast<int>(resolved.error()), static_cast<int>(TrustStoreError::Malformed));
    auto record = RecordKnownHost(broken, "127.0.0.1:1", KeyFilledWith(0x33));
    EXPECT_FALSE(record.has_value());
    EXPECT_EQ(ReadText(broken), std::string("garbage\n"));
}

TEST(Transport_TrustStore_StartServerWithoutIdentityRefused)
{
    auto& manager = NetworkManager::GetInstance();
    manager.Shutdown();

    {
        NetworkSecurityConfig noIdentity = SparkTestFixtures::TestNetworkSecurityConfig();
        noIdentity.identity.reset();
        SparkTestFixtures::ScopedNetworkSecurity scope(std::move(noIdentity));
        EXPECT_FALSE(manager.StartServer(0, 4, NetworkEndpointPolicy::Loopback()));
        EXPECT_EQ(static_cast<int>(manager.GetRole()), static_cast<int>(NetworkRole::None));
        EXPECT_EQ(manager.GetBoundPort(), static_cast<uint16_t>(0));
    }
    {
        // An all-zero pin is "not configured", never "accept anything".
        NetworkSecurityConfig zeroPin = SparkTestFixtures::TestNetworkSecurityConfig();
        zeroPin.trust = ServerTrust::Pin(ServerPublicKey{});
        SparkTestFixtures::ScopedNetworkSecurity scope(std::move(zeroPin));
        EXPECT_FALSE(manager.Connect("127.0.0.1", 27015, "NoTrust", NetworkEndpointPolicy::Loopback()));
        EXPECT_EQ(static_cast<int>(manager.GetRole()), static_cast<int>(NetworkRole::None));
    }
    {
        NetworkSecurityConfig noHosts = SparkTestFixtures::TestNetworkSecurityConfig();
        noHosts.trust = ServerTrust::TrustOnFirstUseAt({});
        SparkTestFixtures::ScopedNetworkSecurity scope(std::move(noHosts));
        EXPECT_FALSE(manager.Connect("127.0.0.1", 27015, "NoTrust", NetworkEndpointPolicy::Loopback()));
    }

    // The restored configuration starts normally, so the refusals above were about security.
    ASSERT_TRUE(manager.StartServer(0, 4, NetworkEndpointPolicy::Loopback()));
    EXPECT_EQ(static_cast<int>(manager.GetRole()), static_cast<int>(NetworkRole::Server));
    manager.Shutdown();
}
