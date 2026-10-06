/**
 * @file TFLanBeaconCodec.cpp
 * @brief LAN discovery beacon decode and server-list merge (socket-free).
 *
 * TFLanDiscovery::UpdateScanner runs these two functions on every datagram
 * received on UDP 27025, which any LAN host can send. They are split from the
 * scanner so the SEC-120 fuzz target (FuzzerTests/FuzzTFLanBeacon.cpp) drives
 * the production decoder without a socket. Thread affinity: none (pure
 * functions over caller-owned data).
 */
#include "Game/TFLanDiscovery.h"

#include <cstring>

namespace Terrafront
{

    std::optional<TF_LanBeacon> DecodeLanBeacon(std::span<const uint8_t> datagram)
    {
        if (datagram.size() != sizeof(TF_LanBeacon))
        {
            return std::nullopt; // not ours (or a future/past size)
        }

        TF_LanBeacon beacon{};
        std::memcpy(&beacon, datagram.data(), sizeof(beacon));
        if (beacon.magic != kTFLanBeaconMagic || beacon.version != kTFLanBeaconVersion || beacon.gamePort == 0)
        {
            return std::nullopt;
        }
        return beacon;
    }

    bool UpsertLanServer(std::vector<TFLanServerEntry>& servers, const TF_LanBeacon& beacon, std::string_view srcIp,
                         double clock)
    {
        // Wire strings are fixed arrays that need not be NUL-terminated: copy up
        // to the first NUL or the end of the array, never past it.
        const auto boundedString = [](const char* text, size_t capacity)
        {
            const void* nul = std::memchr(text, '\0', capacity);
            return std::string(text, nul ? static_cast<size_t>(static_cast<const char*>(nul) - text) : capacity);
        };

        // Dedupe by source IP + advertised game port (two servers on one box on
        // different ports stay distinct; rebroadcasts refresh in place).
        TFLanServerEntry* entry = nullptr;
        for (TFLanServerEntry& existing : servers)
        {
            if (existing.ip == srcIp && existing.gamePort == beacon.gamePort)
            {
                entry = &existing;
                break;
            }
        }
        if (entry == nullptr)
        {
            if (servers.size() >= kTFLanMaxServers)
            {
                return false;
            }
            entry = &servers.emplace_back();
            entry->ip = srcIp;
            entry->gamePort = beacon.gamePort;
        }

        entry->name = boundedString(beacon.serverName, sizeof(beacon.serverName));
        entry->map = boundedString(beacon.mapName, sizeof(beacon.mapName));
        entry->players = beacon.playerCount;
        entry->maxPlayers = beacon.maxPlayers;
        entry->lastSeen = clock;
        return true;
    }

} // namespace Terrafront
