/**
 * @file TFLanDiscovery.cpp
 * @brief W11 server-browser lane: lifecycle + LAN beacon broadcaster half
 *        (see header). The scanner half lives in TFLanDiscoveryScan.cpp; the
 *        shared WinSock/BSD socket shim lives in TFLanDiscoveryInternal.h.
 *
 * Raw, self-contained UDP sockets (WinSock on Windows, BSD sockets elsewhere);
 * NetworkManager is only READ (GetClients() size for the advertised player
 * count) and never modified or routed through.
 */
#include "Game/TFLanDiscovery.h"

#include "Utils/ConsoleVariable.h"
#include "Utils/LogMacros.h"

#include "Data/TFDataTables.h"

#include "Game/TFLanDiscoveryInternal.h"

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

    using namespace LanDetail;

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
        m_endpointPolicy = Spark::Net::CaptureNetworkEndpointPolicy();
        if (!m_endpointPolicy.IsValid())
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game, "[TF] lan: endpoint policy rejected - discovery disabled");
            m_beaconFailed = true;
            m_scanFailed = true;
        }

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

        sockaddr_in localAddress{};
        localAddress.sin_family = AF_INET;
        localAddress.sin_port = 0;
        localAddress.sin_addr.s_addr = htonl(m_endpointPolicy.BindAddress());
        if (bind(s, reinterpret_cast<const sockaddr*>(&localAddress), sizeof(localAddress)) != 0)
        {
            SPARK_LOG_WARN(Spark::LogCategory::Game,
                           "[TF] lan: beacon could not bind the configured interface - LAN advertising disabled");
            CloseSockHandle(s);
            m_beaconFailed = true;
            return;
        }

        const int broadcastOn = 1;
        if ((m_endpointPolicy.PeerScope() == Spark::Net::NetworkPeerScope::PrivateLan &&
             setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&broadcastOn),
                        sizeof(broadcastOn)) != 0) ||
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
        if (m_beaconSock == kInvalidSock)
            return;
        if (m_endpointPolicy.PeerScope() == Spark::Net::NetworkPeerScope::LoopbackOnly)
        {
            m_bcastTargets.push_back(htonl(m_endpointPolicy.BindAddress()));
            return;
        }

        // Select only the explicitly bound RFC1918 interface. Never emit on
        // every interface or fall back to the limited broadcast address.
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
                if (addr != htonl(m_endpointPolicy.BindAddress()))
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
                const uint32_t address = reinterpret_cast<const sockaddr_in*>(it->ifa_addr)->sin_addr.s_addr;
                if (address != htonl(m_endpointPolicy.BindAddress()))
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

} // namespace Terrafront
