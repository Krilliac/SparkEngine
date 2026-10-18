/**
 * @file TFLanDiscovery.h
 * @brief W11 server-browser lane: LAN server discovery (UDP beacon + scanner).
 *
 * OWNERSHIP: this header + TFLanDiscovery.cpp belong to ONE implementation
 * agent (server-browser lane, W11). The instance is OWNED BY TFLoginFlow — no
 * TFGameContext pointer, no Main.cpp wiring: TFLoginFlow::Update runs on every
 * role (Main.cpp calls it unconditionally, dedicated servers included), so it
 * can drive both halves of this feature.
 *
 * BEACON (server side): while ctx.role is ListenHost or DedicatedServer (i.e.
 * tf_host / tf_dedicated ran), the authoritative NetworkManager server option
 * permits LAN advertisement, and the `tf_lan_advertise` preference is true, a
 * raw non-blocking UDP socket emits one TF_LanBeacon every 2 s. Loopback policy
 * uses local unicast; an explicit RFC1918 CIDR policy binds one interface and
 * derives the one directed subnet broadcast from that captured prefix.
 * The beacon carries ONLY public info: name (`tf_server_name` cvar), player
 * count / max, map name, advertised game port (`tf_lan_port` cvar, default
 * 27020 — set it if the server was hosted on a non-default port).
 *
 * SCANNER (client side): TFLoginFlow starts the scanner only while its login /
 * register screen is actually rendering (so headless test runs never bind).
 * The scanner binds UDP 27025 (SO_REUSEADDR so several clients on one box
 * coexist), collects beacons non-blockingly, dedupes by source IP + advertised
 * game port, and expires entries not re-seen for 6 s. The list holds at most
 * kTFLanMaxServers entries, so a LAN peer spoofing many (ip, port) pairs cannot
 * grow it without bound. DecodeLanBeacon and UpsertLanServer
 * (TFLanBeaconCodec.cpp) are the socket-free decode and merge steps the scanner
 * runs on every datagram; the SEC-120 fuzz target drives the same two functions.
 *
 * SAFETY:
 *  - All bind/socket failures are NON-FATAL: one SPARK_LOG_WARN, then the
 *    feature turns itself off for the session (no retry spam).
 *  - Self-contained WinSock/BSD sockets consume NetworkManager's immutable
 *    discovery configuration; they never recapture server configuration.
 *  - Firewall stance: UDP 27025 needs the same LAN-only inbound allowance as
 *    the game port (27020). Do NOT forward either beyond the LAN.
 *  - Headless/test runs: the beacon only starts after tf_host/tf_dedicated
 *    flipped the role; the scanner only starts from the ImGui login screen.
 *  - Compiles to inert stubs when ENABLE_NETWORKING is off.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Core/TFEvents.h"
#include "Engine/Networking/NetworkBindPolicy.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Terrafront
{

    // -----------------------------------------------------------------------
    // Wire format — NOT a TFMsg (rides its own UDP datagram on port 27025,
    // never NetworkManager), but follows the same packed + static_assert +
    // versioned discipline as Net/TFNetProtocol.h.
    // -----------------------------------------------------------------------

    constexpr uint16_t kTFLanBeaconPort = 27025;       ///< UDP destination port for beacons
    constexpr uint32_t kTFLanBeaconMagic = 0x424C4654; ///< bytes "TFLB" on the wire (little-endian)
    constexpr uint16_t kTFLanBeaconVersion = 1;
    constexpr float kTFLanBeaconIntervalSec = 2.0f; ///< server broadcast cadence
    constexpr float kTFLanServerTtlSec = 6.0f;      ///< scanner entry expiry (3 missed beacons)
    constexpr size_t kTFLanMaxServers = 64;         ///< scanner list cap; new servers past it are dropped

#pragma pack(push, 1)
    struct TF_LanBeacon
    {
        uint32_t magic;      // kTFLanBeaconMagic
        uint16_t version;    // kTFLanBeaconVersion; scanner drops mismatches
        uint16_t gamePort;   // game-server port to tf_connect to (host byte order; LAN peers share it)
        uint8_t playerCount; // connected clients (+1 local player on listen hosts)
        uint8_t maxPlayers;  // kMaxPlayers
        uint8_t role;        // NetRole (ListenHost / DedicatedServer), informational
        uint8_t reserved;    // zero; room for future flags without a version bump
        char serverName[32]; // NUL-terminated tf_server_name (public display name only)
        char mapName[24];    // NUL-terminated continent/map name
    };
#pragma pack(pop)
    static_assert(sizeof(TF_LanBeacon) == 68, "TF_LanBeacon wire layout drifted");
    static_assert(sizeof(TF_LanBeacon) <= 96, "TF_LanBeacon must stay <= 96 bytes");

    /// One discovered LAN server (scanner output, already deduped + fresh).
    struct TFLanServerEntry
    {
        std::string ip; ///< beacon source IPv4, dotted quad — what tf_connect uses
        uint16_t gamePort = 0;
        std::string name;
        std::string map;
        uint8_t players = 0;
        uint8_t maxPlayers = 0;
        double lastSeen = 0.0; ///< internal clock stamp (freshness bookkeeping)
    };

    // -----------------------------------------------------------------------
    // Beacon codec (TFLanBeaconCodec.cpp) — pure functions, no sockets, any thread.
    // -----------------------------------------------------------------------

    /// Decode one scanner datagram.
    /// @return The beacon, or nullopt unless @p datagram is exactly sizeof(TF_LanBeacon)
    ///         bytes with kTFLanBeaconMagic, kTFLanBeaconVersion and a nonzero game port.
    std::optional<TF_LanBeacon> DecodeLanBeacon(std::span<const uint8_t> datagram);

    /// Merge a decoded beacon from @p srcIp into @p servers, deduped by (srcIp, gamePort).
    /// The wire name and map are copied up to their first NUL (at most 32 / 24 bytes).
    /// A known server is refreshed in place; a new one is appended only while the list
    /// holds fewer than kTFLanMaxServers entries.
    /// @return True when an entry was refreshed or added, false when the list is full.
    bool UpsertLanServer(std::vector<TFLanServerEntry>& servers, const TF_LanBeacon& beacon, std::string_view srcIp,
                         double clock);

    // -----------------------------------------------------------------------
    // TFLanDiscovery
    // -----------------------------------------------------------------------

    class TFLanDiscovery
    {
      public:
        TFLanDiscovery();
        ~TFLanDiscovery();

        bool Initialize(TFGameContext& ctx, TFEventBus& events);
        void Update(float deltaTime);
        void Shutdown();

        // --- scanner (client side; driven by TFLoginFlow's login screen) ----

        /// Idempotent: binds UDP 27025 on first call. A bind failure logs once
        /// and latches the scanner off for the session (IsScanAvailable()).
        void StartScanning();
        void StopScanning();
        bool IsScanning() const;
        /// False once a scanner socket/bind failure latched the feature off.
        bool IsScanAvailable() const { return !m_scanFailed; }
        /// Fresh, deduped entries (stable order: oldest-discovered first).
        const std::vector<TFLanServerEntry>& Servers() const { return m_servers; }

        // --- beacon (server side; fully self-driving off ctx.role) ----------
        bool IsBeaconActive() const;

      private:
        void UpdateBeacon(float dt);
        void UpdateScanner();
        void StartBeacon();
        void StopBeacon();
        void BroadcastBeacon();
        void FillBeacon(TF_LanBeacon& out) const;
        void RefreshBroadcastTargets();
        void RefreshEndpointConfiguration();

        TFGameContext* m_ctx{nullptr};
        bool m_initialized{false};
        bool m_wsaStarted{false}; // Windows: WSAStartup succeeded (paired WSACleanup in Shutdown)
        Spark::Net::NetworkEndpointPolicy m_endpointPolicy{}; // Same snapshot as the active game socket lifecycle.
        bool m_allowAdvertisement{false};                     // Authoritative Network.lan_broadcast gate.
        double m_clock{0.0};                                  // monotonic feature clock (drives TTL expiry)

        // Sockets stored type-erased so this header never includes WinSock;
        // -1 == invalid on both platforms (INVALID_SOCKET is ~0 == -1 as intptr).
        intptr_t m_beaconSock{-1};
        intptr_t m_scanSock{-1};

        // beacon state
        float m_beaconTimer{0.0f};
        float m_targetRefreshTimer{0.0f};
        bool m_beaconFailed{false};           // latched off after one logged failure
        std::vector<uint32_t> m_bcastTargets; // network-order policy-scoped discovery destinations

        // scanner state
        bool m_scanFailed{false}; // latched off after one logged failure
        std::vector<TFLanServerEntry> m_servers;
    };

} // namespace Terrafront
