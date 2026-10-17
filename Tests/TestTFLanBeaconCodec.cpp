/**
 * @file TestTFLanBeaconCodec.cpp
 * @brief SEC-120: TERRAFRONT LAN discovery beacon decode and server-list merge.
 *
 * Any LAN host can send a datagram to the scanner's UDP 27025, so the decode is
 * an untrusted-network parser. Game/TFLanBeaconCodec.cpp holds the two pure
 * functions TFLanDiscovery::UpdateScanner runs on every datagram; it is compiled
 * into SparkTests explicitly (the TFServerValidation.cpp pattern).
 */
#include "TestFramework.h"

#include "Game/TFLanDiscovery.h"

#include <cstring>
#include <string>
#include <vector>

using namespace Terrafront;

namespace
{
    TF_LanBeacon ValidBeacon(uint16_t gamePort = 27020)
    {
        TF_LanBeacon beacon{};
        beacon.magic = kTFLanBeaconMagic;
        beacon.version = kTFLanBeaconVersion;
        beacon.gamePort = gamePort;
        beacon.playerCount = 3;
        beacon.maxPlayers = 16;
        std::memcpy(beacon.serverName, "Lan Server", sizeof("Lan Server"));
        std::memcpy(beacon.mapName, "Ashfall", sizeof("Ashfall"));
        return beacon;
    }

    std::vector<uint8_t> Bytes(const TF_LanBeacon& beacon)
    {
        std::vector<uint8_t> bytes(sizeof(beacon));
        std::memcpy(bytes.data(), &beacon, sizeof(beacon));
        return bytes;
    }
} // namespace

TEST(TFLanBeacon_DecodeRejectsWrongSizeMagicVersionAndZeroPort)
{
    const std::vector<uint8_t> valid = Bytes(ValidBeacon());
    const auto decoded = DecodeLanBeacon(valid);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->gamePort, static_cast<uint16_t>(27020));
    EXPECT_EQ(decoded->playerCount, static_cast<uint8_t>(3));

    std::vector<uint8_t> shortByOne(valid.begin(), valid.end() - 1);
    EXPECT_FALSE(DecodeLanBeacon(shortByOne).has_value());
    std::vector<uint8_t> longByOne = valid;
    longByOne.push_back(0);
    EXPECT_FALSE(DecodeLanBeacon(longByOne).has_value());
    EXPECT_FALSE(DecodeLanBeacon({}).has_value());

    TF_LanBeacon wrongMagic = ValidBeacon();
    wrongMagic.magic ^= 1u;
    EXPECT_FALSE(DecodeLanBeacon(Bytes(wrongMagic)).has_value());

    TF_LanBeacon wrongVersion = ValidBeacon();
    wrongVersion.version = static_cast<uint16_t>(kTFLanBeaconVersion + 1);
    EXPECT_FALSE(DecodeLanBeacon(Bytes(wrongVersion)).has_value());

    EXPECT_FALSE(DecodeLanBeacon(Bytes(ValidBeacon(0))).has_value());
}

TEST(TFLanBeacon_UpsertCapsServerListAtLimit)
{
    std::vector<TFLanServerEntry> servers;
    // One spoofing host advertises 65 distinct game ports: every (ip, port) pair is a new server.
    for (uint16_t port = 1; port <= kTFLanMaxServers + 1; ++port)
    {
        const bool stored = UpsertLanServer(servers, ValidBeacon(port), "192.168.1.50", 1.0);
        EXPECT_EQ(stored, port <= kTFLanMaxServers);
    }
    EXPECT_EQ(servers.size(), kTFLanMaxServers);

    // A full list still refreshes a server it already holds.
    TF_LanBeacon refresh = ValidBeacon(1);
    refresh.playerCount = 9;
    EXPECT_TRUE(UpsertLanServer(servers, refresh, "192.168.1.50", 2.0));
    EXPECT_EQ(servers.size(), kTFLanMaxServers);
    EXPECT_EQ(servers.front().players, static_cast<uint8_t>(9));
    EXPECT_NEAR(servers.front().lastSeen, 2.0, 1e-9);

    // The same port from another host is a different server, so it is refused too.
    EXPECT_FALSE(UpsertLanServer(servers, ValidBeacon(1), "192.168.1.51", 2.0));
    EXPECT_EQ(servers.size(), kTFLanMaxServers);
}

TEST(TFLanBeacon_UnterminatedNamesAreBounded)
{
    TF_LanBeacon beacon = ValidBeacon();
    std::memset(beacon.serverName, 'N', sizeof(beacon.serverName));
    std::memset(beacon.mapName, 'M', sizeof(beacon.mapName));

    const auto decoded = DecodeLanBeacon(Bytes(beacon));
    ASSERT_TRUE(decoded.has_value());
    std::vector<TFLanServerEntry> servers;
    ASSERT_TRUE(UpsertLanServer(servers, *decoded, "10.0.0.7", 0.5));
    ASSERT_EQ(servers.size(), static_cast<size_t>(1));
    EXPECT_TRUE(servers[0].name == std::string(sizeof(beacon.serverName), 'N'));
    EXPECT_TRUE(servers[0].map == std::string(sizeof(beacon.mapName), 'M'));

    // An embedded NUL ends the name; bytes after it never reach the list.
    std::memcpy(beacon.serverName, "ab\0cd", 5);
    ASSERT_TRUE(UpsertLanServer(servers, beacon, "10.0.0.7", 0.6));
    ASSERT_EQ(servers.size(), static_cast<size_t>(1));
    EXPECT_TRUE(servers[0].name == "ab");
    EXPECT_TRUE(servers[0].ip == "10.0.0.7");
}
