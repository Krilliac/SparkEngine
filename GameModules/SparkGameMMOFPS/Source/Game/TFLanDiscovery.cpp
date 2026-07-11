/**
 * @file TFLanDiscovery.cpp
 * @brief W11 server-browser lane: LAN beacon broadcaster + scanner (see header).
 *
 * Raw, self-contained UDP sockets (WinSock on Windows, BSD sockets elsewhere);
 * NetworkManager is only READ (GetClients() size for the advertised player
 * count) and never modified or routed through.
 */
#include "Game/TFLanDiscovery.h"

#include "Utils/ConsoleVariable.h"
#include "Utils/LogMacros.h"

#include "Data/TFDataTables.h"

#ifdef ENABLE_NETWORKING
#include "Engine/Networking/NetworkManager.h" // read-only: client count for the beacon

#ifdef SPARK_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ws2ipdef.h> // INTERFACE_INFO / SIO_GET_INTERFACE_LIST
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#endif // ENABLE_NETWORKING

#include <algorithm>
#include <cstring>

namespace Terrafront
{

    namespace
    {

        // ------------------------------------------------------------------
        // Console variables. Registered in THIS image's CVarRegistry — the same
        // image whose SimpleConsole executes tf_* commands (module DLL statics
        // are per-image; both the console input path and these cvars live on
        // the module side, so `tf_server_name My Server` resolves normally).
        // ------------------------------------------------------------------

        Spark::CVar<std::string> cv_tfServerName("tf_server_name", "TERRAFRONT Server", Spark::CVarFlags::Save,
                                                 "Public display name broadcast in the LAN server-browser beacon");

        Spark::CVar<bool> cv_tfLanAdvertise("tf_lan_advertise", true, Spark::CVarFlags::Save,
                                            "Broadcast a LAN discovery beacon (UDP 27025) while hosting");

        Spark::CVar<int> cv_tfLanPort("tf_lan_port", 27020, Spark::CVarFlags::Save,
                                      "Game port advertised in the LAN beacon (set to match tf_host/tf_dedicated "
                                      "when hosting on a non-default port)",
                                      1, 65535);

#ifdef ENABLE_NETWORKING

        constexpr float kTargetRefreshSec = 30.0f; // re-enumerate interface broadcasts
        constexpr int kMaxDatagramsPerTick = 64;   // scanner drain cap per Update

#ifdef SPARK_PLATFORM_WINDOWS
        using TFSockHandle = SOCKET;

        TFSockHandle ToSock(intptr_t s)
        {
            return static_cast<TFSockHandle>(s);
        }

        void CloseSockHandle(TFSockHandle s)
        {
            closesocket(s);
        }

        bool SetNonBlocking(TFSockHandle s)
        {
            u_long nonBlocking = 1;
            return ioctlsocket(s, FIONBIO, &nonBlocking) == 0;
        }

        bool WouldBlock()
        {
            const int e = WSAGetLastError();
            return e == WSAEWOULDBLOCK;
        }

        bool IgnorableRecvError()
        {
            // UDP sockets surface ICMP port-unreachable from an earlier send as
            // WSAECONNRESET on recvfrom — not an error for a broadcast listener.
            const int e = WSAGetLastError();
            return e == WSAECONNRESET || e == WSAEMSGSIZE;
        }
#else
        using TFSockHandle = int;

        TFSockHandle ToSock(intptr_t s)
        {
            return static_cast<TFSockHandle>(s);
        }

        void CloseSockHandle(TFSockHandle s)
        {
            ::close(s);
        }

        bool SetNonBlocking(TFSockHandle s)
        {
            const int flags = fcntl(s, F_GETFL, 0);
            return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
        }

        bool WouldBlock()
        {
            return errno == EWOULDBLOCK || errno == EAGAIN;
        }

        bool IgnorableRecvError()
        {
            return errno == ECONNREFUSED || errno == EINTR;
        }
#endif

        constexpr intptr_t kInvalidSock = -1;

#endif // ENABLE_NETWORKING

    } // namespace

    TFLanDiscovery::TFLanDiscovery() = default;

    TFLanDiscovery::~TFLanDiscovery()
    {
        if (m_initialized)
            Shutdown();
    }

    bool TFLanDiscovery::Initialize(TFGameContext& ctx, TFEventBus& events)
    {
        (void)events; // no bus traffic: everything is polled off ctx.role
        m_ctx = &ctx;
        m_clock = 0.0;
        m_beaconTimer = 0.0f;
        m_targetRefreshTimer = 0.0f;
        m_beaconFailed = false;
        m_scanFailed = false;
        m_servers.clear();

#ifdef ENABLE_NETWORKING
#ifdef SPARK_PLATFORM_WINDOWS
        // Ref-counted by the OS; safe alongside NetworkManager's own WSAStartup.
        WSADATA wsaData{};
        m_wsaStarted = (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0);
        if (!m_wsaStarted)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] lan: WSAStartup failed - LAN discovery disabled");
            m_beaconFailed = true;
            m_scanFailed = true;
        }
#endif
#else
        // No sockets in this build: latch both halves off quietly.
        m_beaconFailed = true;
        m_scanFailed = true;
#endif

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] TFLanDiscovery initialized (beacon port %u)",
                       static_cast<unsigned>(kTFLanBeaconPort));
        return true;
    }

    void TFLanDiscovery::Shutdown()
    {
        if (!m_initialized)
            return;
        StopBeacon();
        StopScanning();
        m_servers.clear();
#if defined(ENABLE_NETWORKING) && defined(SPARK_PLATFORM_WINDOWS)
        if (m_wsaStarted)
        {
            WSACleanup();
            m_wsaStarted = false;
        }
#endif
        m_initialized = false;
    }

    void TFLanDiscovery::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        m_clock += static_cast<double>(deltaTime);
        UpdateBeacon(deltaTime);
        UpdateScanner();

        // Expire entries that missed 3 consecutive beacons.
        std::erase_if(m_servers, [this](const TFLanServerEntry& e)
                      { return m_clock - e.lastSeen > static_cast<double>(kTFLanServerTtlSec); });
    }

    bool TFLanDiscovery::IsBeaconActive() const
    {
#ifdef ENABLE_NETWORKING
        return m_beaconSock != kInvalidSock;
#else
        return false;
#endif
    }

    bool TFLanDiscovery::IsScanning() const
    {
#ifdef ENABLE_NETWORKING
        return m_scanSock != kInvalidSock;
#else
        return false;
#endif
    }

    // -----------------------------------------------------------------------
    // Beacon (server side)
    // -----------------------------------------------------------------------

    void TFLanDiscovery::UpdateBeacon(float dt)
    {
#ifdef ENABLE_NETWORKING
        const NetRole role = m_ctx ? m_ctx->role : NetRole::Standalone;
        const bool hosting = (role == NetRole::ListenHost || role == NetRole::DedicatedServer);
        const bool want = hosting && cv_tfLanAdvertise.Get() && !m_beaconFailed;

        if (want && m_beaconSock == kInvalidSock)
            StartBeacon();
        else if (!want && m_beaconSock != kInvalidSock)
            StopBeacon();

        if (m_beaconSock == kInvalidSock)
            return;

        m_targetRefreshTimer += dt;
        if (m_targetRefreshTimer >= kTargetRefreshSec)
        {
            m_targetRefreshTimer = 0.0f;
            RefreshBroadcastTargets();
        }

        m_beaconTimer += dt;
        if (m_beaconTimer >= kTFLanBeaconIntervalSec)
        {
            m_beaconTimer = 0.0f;
            BroadcastBeacon();
        }
#else
        (void)dt;
#endif
    }

    void TFLanDiscovery::StartBeacon()
    {
#ifdef ENABLE_NETWORKING
        const TFSockHandle s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef SPARK_PLATFORM_WINDOWS
        const bool created = (s != INVALID_SOCKET);
#else
        const bool created = (s >= 0);
#endif
        if (!created)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] lan: beacon socket() failed - LAN advertising disabled");
            m_beaconFailed = true;
            return;
        }

        const int broadcastOn = 1;
        if (setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcastOn), sizeof(broadcastOn)) !=
                0 ||
            !SetNonBlocking(s))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] lan: beacon socket setup failed - LAN advertising disabled");
            CloseSockHandle(s);
            m_beaconFailed = true;
            return;
        }

        m_beaconSock = static_cast<intptr_t>(s);
        m_beaconTimer = 0.0f;
        m_targetRefreshTimer = 0.0f;
        RefreshBroadcastTargets();
        BroadcastBeacon(); // announce immediately; then every 2 s

        SPARK_LOG_INFO(Spark::LogCategory::Game,
                       "[TF] lan: advertising '%s' on UDP %u (LAN-only; firewall stance = same as the game port)",
                       cv_tfServerName.Get().c_str(), static_cast<unsigned>(kTFLanBeaconPort));
#endif
    }

    void TFLanDiscovery::StopBeacon()
    {
#ifdef ENABLE_NETWORKING
        if (m_beaconSock == kInvalidSock)
            return;
        CloseSockHandle(ToSock(m_beaconSock));
        m_beaconSock = kInvalidSock;
        m_bcastTargets.clear();
        SPARK_LOG_INFO(Spark::LogCategory::Game, "[TF] lan: beacon stopped");
#endif
    }

    void TFLanDiscovery::RefreshBroadcastTargets()
    {
#ifdef ENABLE_NETWORKING
        m_bcastTargets.clear();
        m_bcastTargets.push_back(INADDR_BROADCAST); // 255.255.255.255 (limited broadcast)

        if (m_beaconSock == kInvalidSock)
            return;

            // Directed subnet broadcasts per up, broadcast-capable, non-loopback
            // interface — some LANs/adapters drop 255.255.255.255 but pass these.
#ifdef SPARK_PLATFORM_WINDOWS
        INTERFACE_INFO infos[32]{};
        DWORD bytes = 0;
        if (WSAIoctl(ToSock(m_beaconSock), SIO_GET_INTERFACE_LIST, nullptr, 0, infos, static_cast<DWORD>(sizeof(infos)),
                     &bytes, nullptr, nullptr) == 0)
        {
            const size_t count = static_cast<size_t>(bytes) / sizeof(INTERFACE_INFO);
            for (size_t i = 0; i < count; ++i)
            {
                const u_long flags = infos[i].iiFlags;
                if (!(flags & IFF_UP) || !(flags & IFF_BROADCAST) || (flags & IFF_LOOPBACK))
                    continue;
                const uint32_t addr = infos[i].iiAddress.AddressIn.sin_addr.s_addr;
                const uint32_t mask = infos[i].iiNetmask.AddressIn.sin_addr.s_addr;
                if (addr == 0)
                    continue;
                const uint32_t bcast = (addr & mask) | ~mask; // network byte order throughout
                if (std::find(m_bcastTargets.begin(), m_bcastTargets.end(), bcast) == m_bcastTargets.end())
                    m_bcastTargets.push_back(bcast);
            }
        }
#else
        ifaddrs* list = nullptr;
        if (getifaddrs(&list) == 0)
        {
            for (const ifaddrs* it = list; it; it = it->ifa_next)
            {
                if (!it->ifa_addr || it->ifa_addr->sa_family != AF_INET || !it->ifa_broadaddr)
                    continue;
                if (!(it->ifa_flags & IFF_UP) || !(it->ifa_flags & IFF_BROADCAST) || (it->ifa_flags & IFF_LOOPBACK))
                    continue;
                const uint32_t bcast = reinterpret_cast<const sockaddr_in*>(it->ifa_broadaddr)->sin_addr.s_addr;
                if (std::find(m_bcastTargets.begin(), m_bcastTargets.end(), bcast) == m_bcastTargets.end())
                    m_bcastTargets.push_back(bcast);
            }
            freeifaddrs(list);
        }
#endif
#endif // ENABLE_NETWORKING
    }

    void TFLanDiscovery::FillBeacon(TF_LanBeacon& out) const
    {
        std::memset(&out, 0, sizeof(out));
        out.magic = kTFLanBeaconMagic;
        out.version = kTFLanBeaconVersion;
        out.gamePort = static_cast<uint16_t>(cv_tfLanPort.Get());
        out.maxPlayers = static_cast<uint8_t>(std::min<uint32_t>(kMaxPlayers, 255u));
        out.role = m_ctx ? static_cast<uint8_t>(m_ctx->role) : 0;

        uint32_t players = 0;
#ifdef ENABLE_NETWORKING
        {
            auto& nm = Spark::Net::NetworkManager::GetInstance();
            if (nm.IsInitialized())
                players = static_cast<uint32_t>(nm.GetClients().size());
        }
#endif
        // The listen host's local player rides the loopback router, not a socket.
        if (m_ctx && m_ctx->role == NetRole::ListenHost)
            ++players;
        out.playerCount = static_cast<uint8_t>(std::min<uint32_t>(players, 255u));

        // Public info ONLY beyond this point: display name + map name.
        std::strncpy(out.serverName, cv_tfServerName.Get().c_str(), sizeof(out.serverName) - 1);

        const char* map = "cindral_wastes";
        if (m_ctx && m_ctx->data && m_ctx->data->IsLoaded() && !m_ctx->data->GetContinent().name.empty())
            map = m_ctx->data->GetContinent().name.c_str();
        std::strncpy(out.mapName, map, sizeof(out.mapName) - 1);
    }

    void TFLanDiscovery::BroadcastBeacon()
    {
#ifdef ENABLE_NETWORKING
        if (m_beaconSock == kInvalidSock)
            return;

        TF_LanBeacon beacon{};
        FillBeacon(beacon);

        for (const uint32_t target : m_bcastTargets)
        {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_port = htons(kTFLanBeaconPort);
            to.sin_addr.s_addr = target;
            // Best-effort: a dropped beacon self-heals on the next 2 s tick, so
            // send errors (including EWOULDBLOCK) are deliberately ignored.
            (void)sendto(ToSock(m_beaconSock), reinterpret_cast<const char*>(&beacon), static_cast<int>(sizeof(beacon)),
                         0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
        }
#endif
    }

    // -----------------------------------------------------------------------
    // Scanner (client side)
    // -----------------------------------------------------------------------

    void TFLanDiscovery::StartScanning()
    {
#ifdef ENABLE_NETWORKING
        if (m_scanSock != kInvalidSock || m_scanFailed)
            return;

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
        bindAddr.sin_addr.s_addr = INADDR_ANY;
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
