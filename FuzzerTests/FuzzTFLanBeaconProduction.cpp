/**
 * @file FuzzTFLanBeaconProduction.cpp
 * @brief libc++-compiled production adapter for the TERRAFRONT LAN beacon libFuzzer harness.
 *
 * The input is a sequence of framed datagrams: a length byte, a source byte that
 * picks one of eight fixed LAN addresses, then that many payload bytes (a short
 * final frame keeps what is left). Each datagram goes through the shipped
 * Terrafront::DecodeLanBeacon and, when accepted, Terrafront::UpsertLanServer --
 * the two functions TFLanDiscovery::UpdateScanner runs on every UDP datagram.
 * A violation of their contract aborts so libFuzzer records a crash:
 *  - an accepted datagram is exactly sizeof(TF_LanBeacon) bytes with the current
 *    magic and version and a nonzero game port (checked from the raw bytes),
 *  - the list never exceeds kTFLanMaxServers and never holds two entries for
 *    one (ip, gamePort); a refused beacon means the list was full and the
 *    server was new, and it leaves the list unchanged,
 *  - every stored name and map is the wire field up to its first NUL: at most
 *    32 / 24 bytes and never containing a NUL.
 */

#include "FuzzTFLanBeaconProduction.h"

#include "Game/TFLanDiscovery.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    // 65 framed beacons (70 bytes each) must fit, so a single input can fill the list past kTFLanMaxServers.
    constexpr std::size_t kMaxInputBytes = 8192;

    constexpr std::array<std::string_view, 8> kSourceIps = {
        "192.168.1.10", "192.168.1.11", "192.168.1.12", "10.0.0.2", "10.0.0.3", "172.16.4.1", "172.16.4.2", "127.0.0.1",
    };

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzTFLanBeacon: LAN beacon contract violated: %s\n", what);
        std::abort();
    }

    template <typename T> T ReadLittleEndian(const std::uint8_t* bytes)
    {
        T value = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            value = static_cast<T>(value | (static_cast<T>(bytes[i]) << (8u * i)));
        return value;
    }

    /// The expected stored text for a fixed wire field, computed without the production helper.
    std::string ExpectedField(const char* field, std::size_t capacity)
    {
        std::size_t length = 0;
        while (length < capacity && field[length] != '\0')
            ++length;
        return std::string(field, length);
    }

    const Terrafront::TFLanServerEntry* Find(const std::vector<Terrafront::TFLanServerEntry>& servers,
                                             std::string_view ip, std::uint16_t port)
    {
        const Terrafront::TFLanServerEntry* found = nullptr;
        for (const Terrafront::TFLanServerEntry& entry : servers)
        {
            if (entry.ip == ip && entry.gamePort == port)
            {
                if (found != nullptr)
                    InvariantFailure("two entries share one (ip, gamePort)");
                found = &entry;
            }
        }
        return found;
    }

    void CheckList(const std::vector<Terrafront::TFLanServerEntry>& servers)
    {
        if (servers.size() > Terrafront::kTFLanMaxServers)
            InvariantFailure("the server list exceeds kTFLanMaxServers");
        for (const Terrafront::TFLanServerEntry& entry : servers)
        {
            (void)Find(servers, entry.ip, entry.gamePort);
            if (entry.gamePort == 0)
                InvariantFailure("a stored server has game port 0");
            if (entry.name.size() > sizeof(Terrafront::TF_LanBeacon::serverName) ||
                entry.map.size() > sizeof(Terrafront::TF_LanBeacon::mapName))
                InvariantFailure("a stored name or map is longer than its wire field");
            if (entry.name.find('\0') != std::string::npos || entry.map.find('\0') != std::string::npos)
                InvariantFailure("a stored name or map contains a NUL");
        }
    }

    void FeedDatagram(std::vector<Terrafront::TFLanServerEntry>& servers, std::span<const std::uint8_t> datagram,
                      std::string_view ip, double clock)
    {
        const std::optional<Terrafront::TF_LanBeacon> beacon = Terrafront::DecodeLanBeacon(datagram);
        if (!beacon)
            return;

        if (datagram.size() != sizeof(Terrafront::TF_LanBeacon))
            InvariantFailure("a datagram that is not sizeof(TF_LanBeacon) bytes was accepted");
        if (ReadLittleEndian<std::uint32_t>(datagram.data()) != Terrafront::kTFLanBeaconMagic ||
            ReadLittleEndian<std::uint16_t>(datagram.data() + 4) != Terrafront::kTFLanBeaconVersion)
            InvariantFailure("a datagram with the wrong magic or version was accepted");
        const std::uint16_t port = ReadLittleEndian<std::uint16_t>(datagram.data() + 6);
        if (port == 0 || beacon->gamePort != port)
            InvariantFailure("the accepted game port is zero or differs from the wire");

        const std::size_t before = servers.size();
        const bool known = Find(servers, ip, port) != nullptr;
        if (!Terrafront::UpsertLanServer(servers, *beacon, ip, clock))
        {
            if (known || before < Terrafront::kTFLanMaxServers)
                InvariantFailure("a beacon was refused while the list had room or already held the server");
            if (servers.size() != before)
                InvariantFailure("a refused beacon changed the list");
            return;
        }

        if (servers.size() != (known ? before : before + 1))
            InvariantFailure("an upsert did not refresh in place or append exactly one entry");
        const Terrafront::TFLanServerEntry* entry = Find(servers, ip, port);
        if (entry == nullptr)
            InvariantFailure("an accepted beacon is missing from the list");
        if (entry->name != ExpectedField(beacon->serverName, sizeof(beacon->serverName)) ||
            entry->map != ExpectedField(beacon->mapName, sizeof(beacon->mapName)))
            InvariantFailure("a stored name or map is not the wire field up to its first NUL");
        if (entry->players != beacon->playerCount || entry->maxPlayers != beacon->maxPlayers ||
            entry->lastSeen != clock)
            InvariantFailure("an accepted beacon did not refresh the entry");
    }
} // namespace

// This C ABI is the only boundary between the libstdc++ libFuzzer executable
// and the libc++-compiled production decoder.
extern "C" int SparkFuzzDecodeTFLanBeacon(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes)
        return 0;
    if (data == nullptr && size != 0)
        return 0;

    std::vector<Terrafront::TFLanServerEntry> servers;
    std::size_t offset = 0;
    double clock = 0.0;
    while (size - offset >= 2)
    {
        const std::size_t declared = data[offset];
        const std::string_view ip = kSourceIps[data[offset + 1] % kSourceIps.size()];
        offset += 2;
        const std::size_t length = declared < size - offset ? declared : size - offset;
        FeedDatagram(servers, std::span<const std::uint8_t>(data + offset, length), ip, clock);
        CheckList(servers);
        offset += length;
        clock += 1.0;
    }
    return 0;
}
