/**
 * @file TFLanDiscoveryScan.cpp
 * @brief W11 server-browser lane: LAN scanner half (client side) — bind UDP
 *        27025, drain beacons non-blockingly, validate/dedupe them into the
 *        server list. Split from TFLanDiscovery.cpp; the shared WinSock/BSD
 *        socket shim lives in TFLanDiscoveryInternal.h.
 */
#include "Game/TFLanDiscovery.h"

#include "Engine/Networking/NetworkBindPolicy.h"
#include "Utils/LogMacros.h"

#include "Game/TFLanDiscoveryInternal.h"

#ifdef ENABLE_NETWORKING
#ifdef SPARK_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
#endif // ENABLE_NETWORKING

#include <cstring>

namespace Terrafront
{

    using namespace LanDetail;

    namespace
    {

#ifdef ENABLE_NETWORKING
        constexpr int kMaxDatagramsPerTick = 64; // scanner drain cap per Update
#endif

    } // namespace

    // -----------------------------------------------------------------------
    // Scanner (client side)
    // -----------------------------------------------------------------------

    void TFLanDiscovery::StartScanning()
    {
#ifdef ENABLE_NETWORKING
        if (m_scanSock != kInvalidSock || m_scanFailed)
            return;
        if (!m_endpointPolicy.IsValid())
        {
            m_scanFailed = true;
            return;
        }

        const TFSockHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef SPARK_PLATFORM_WINDOWS
        const bool created = (s != INVALID_SOCKET);
#else
        const bool created = (s >= 0);
#endif
        if (!created)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] lan: scanner socket() failed - LAN server list disabled");
            m_scanFailed = true;
            return;
        }

        // SO_REUSEADDR: several clients on one box (or a client next to a local
        // dedicated server test) may all listen for beacons on 27025.
        const int reuse = 1;
        (void)setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

        sockaddr_in bindAddr{};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_port = htons(kTFLanBeaconPort);
        bindAddr.sin_addr.s_addr = htonl(m_endpointPolicy.BindAddress());
        if (bind(s, reinterpret_cast<const sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0 || !SetNonBlocking(s))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] lan: bind on UDP %u failed - LAN server list disabled for this session",
                           static_cast<unsigned>(kTFLanBeaconPort));
            CloseSockHandle(s);
            m_scanFailed = true;
            return;
        }

        m_scanSock = static_cast<intptr_t>(s);
        m_servers.clear();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] lan: scanning for servers on UDP %u",
                       static_cast<unsigned>(kTFLanBeaconPort));
#endif
    }

    void TFLanDiscovery::StopScanning()
    {
#ifdef ENABLE_NETWORKING
        if (m_scanSock == kInvalidSock)
            return;
        CloseSockHandle(ToSock(m_scanSock));
        m_scanSock = kInvalidSock;
        m_servers.clear();
#endif
    }

    void TFLanDiscovery::UpdateScanner()
    {
#ifdef ENABLE_NETWORKING
        if (m_scanSock == kInvalidSock)
            return;

        for (int i = 0; i < kMaxDatagramsPerTick; ++i)
        {
            char buf[128];
            sockaddr_in from{};
#ifdef SPARK_PLATFORM_WINDOWS
            int fromLen = static_cast<int>(sizeof(from));
            const int n = recvfrom(ToSock(m_scanSock), buf, static_cast<int>(sizeof(buf)), 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
#else
            socklen_t fromLen = static_cast<socklen_t>(sizeof(from));
            const ssize_t n =
                recvfrom(ToSock(m_scanSock), buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
#endif
            if (n < 0)
            {
                if (WouldBlock())
                    break;
                if (IgnorableRecvError())
                    continue;
                break;
            }
            if (from.sin_family != AF_INET || !m_endpointPolicy.AllowsPeerAddress(ntohl(from.sin_addr.s_addr)))
                continue;
            if (static_cast<size_t>(n) != sizeof(TF_LanBeacon))
                continue; // not ours (or a future/past size) — ignore silently

            TF_LanBeacon beacon{};
            std::memcpy(&beacon, buf, sizeof(beacon));
            if (beacon.magic != kTFLanBeaconMagic || beacon.version != kTFLanBeaconVersion || beacon.gamePort == 0)
                continue;

            char srcIp[INET_ADDRSTRLEN]{};
            if (!inet_ntop(AF_INET, &from.sin_addr, srcIp, sizeof(srcIp)))
                continue;

            HandleDatagram(beacon, srcIp);
        }
#endif
    }

    void TFLanDiscovery::HandleDatagram(const TF_LanBeacon& b, const char* srcIp)
    {
        // Defensive NUL-termination — never trust wire strings.
        char name[sizeof(b.serverName) + 1];
        std::memcpy(name, b.serverName, sizeof(b.serverName));
        name[sizeof(b.serverName)] = '\0';
        char map[sizeof(b.mapName) + 1];
        std::memcpy(map, b.mapName, sizeof(b.mapName));
        map[sizeof(b.mapName)] = '\0';

        // Dedupe by source IP + advertised game port (two servers on one box on
        // different ports stay distinct; rebroadcasts refresh in place).
        for (TFLanServerEntry& e : m_servers)
        {
            if (e.ip == srcIp && e.gamePort == b.gamePort)
            {
                e.name = name;
                e.map = map;
                e.players = b.playerCount;
                e.maxPlayers = b.maxPlayers;
                e.lastSeen = m_clock;
                return;
            }
        }

        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] lan: discovered server '%s' at %s:%u", name, srcIp,
                       static_cast<unsigned>(b.gamePort));

        TFLanServerEntry entry;
        entry.ip = srcIp;
        entry.gamePort = b.gamePort;
        entry.name = name;
        entry.map = map;
        entry.players = b.playerCount;
        entry.maxPlayers = b.maxPlayers;
        entry.lastSeen = m_clock;
        m_servers.push_back(std::move(entry));
    }

} // namespace Terrafront
