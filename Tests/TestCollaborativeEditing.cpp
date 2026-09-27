/**
 * @file TestCollaborativeEditing.cpp
 * @brief Tests for the collaborative editing system
 *
 * Tests cover: session lifecycle, serialization, locking, edit broadcasting,
 * peer management, and the live edit bridge.
 */

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "TestFramework.h"
#include "Communication/CollaborativeEditSession.h"
#include "Communication/LiveEditBridge.h"
#include "Engine/Networking/NetworkBindPolicy.h"
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace SparkEditor;

namespace
{
#ifdef _WIN32
    using TestSocketHandle = SOCKET;
    constexpr TestSocketHandle INVALID_TEST_SOCKET = INVALID_SOCKET;

    void CloseTestSocket(TestSocketHandle socket)
    {
        if (socket != INVALID_TEST_SOCKET)
            ::closesocket(socket);
    }
#else
    using TestSocketHandle = int;
    constexpr TestSocketHandle INVALID_TEST_SOCKET = -1;

    void CloseTestSocket(TestSocketHandle socket)
    {
        if (socket != INVALID_TEST_SOCKET)
            ::close(socket);
    }
#endif

    struct RefusedPortReservation
    {
        ~RefusedPortReservation() { CloseTestSocket(socket); }

        bool Reserve()
        {
            socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_TEST_SOCKET)
                return false;

            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(0);
            if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                return false;

#ifdef _WIN32
            int addressLength = sizeof(address);
#else
            socklen_t addressLength = sizeof(address);
#endif
            if (::getsockname(socket, reinterpret_cast<sockaddr*>(&address), &addressLength) != 0)
                return false;

            port = ntohs(address.sin_port);
            return port != 0;
        }

        bool Listen() const { return ::listen(socket, 1) == 0; }

        TestSocketHandle socket = INVALID_TEST_SOCKET;
        uint16_t port = 0;
    };

    // A well-formed join code that no host generated.
    const std::string kForeignJoinCode(kCollabJoinSecretBytes * 2, 'a');

    /// Raw TCP peer that speaks the legacy collaboration wire format directly, so
    /// tests can drive the host's trust boundary without a well-behaved client.
    struct RawCollabPeer
    {
        enum class RecvStatus
        {
            Frame,
            Closed,
            TimedOut
        };

        ~RawCollabPeer() { CloseTestSocket(socket); }

        bool Connect(uint16_t port)
        {
            socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket == INVALID_TEST_SOCKET)
                return false;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
                return false;
#ifdef _WIN32
            const DWORD timeoutMs = 250;
            ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeoutMs), sizeof(timeoutMs));
#else
            timeval tv{0, 250000};
            ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            return true;
        }

        bool SendRaw(const void* data, size_t size)
        {
            const auto* bytes = static_cast<const char*>(data);
            size_t sent = 0;
            while (sent < size)
            {
                const auto n = ::send(socket, bytes + sent, static_cast<int>(size - sent), 0);
                if (n <= 0)
                    return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        bool SendHeader(uint32_t length)
        {
            const uint8_t header[4] = {static_cast<uint8_t>(length >> 24), static_cast<uint8_t>(length >> 16),
                                       static_cast<uint8_t>(length >> 8), static_cast<uint8_t>(length)};
            return SendRaw(header, sizeof(header));
        }

        bool SendInternal(const InternalMessage& msg)
        {
            const auto bytes = SerializeMessage(msg);
            return SendHeader(static_cast<uint32_t>(bytes.size())) && SendRaw(bytes.data(), bytes.size());
        }

        // Reads exactly size bytes; distinguishes an orderly/abortive close from a timeout.
        RecvStatus RecvExact(void* data, size_t size, std::chrono::milliseconds timeout)
        {
            auto* bytes = static_cast<char*>(data);
            size_t received = 0;
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (received < size)
            {
                const auto n = ::recv(socket, bytes + received, static_cast<int>(size - received), 0);
                if (n > 0)
                {
                    received += static_cast<size_t>(n);
                    continue;
                }
                if (n == 0)
                    return RecvStatus::Closed;
#ifdef _WIN32
                const bool timedOut = WSAGetLastError() == WSAETIMEDOUT;
#else
                const bool timedOut = errno == EAGAIN || errno == EWOULDBLOCK;
#endif
                if (!timedOut)
                    return RecvStatus::Closed; // reset by the host counts as a close
                if (std::chrono::steady_clock::now() > deadline)
                    return RecvStatus::TimedOut;
            }
            return RecvStatus::Frame;
        }

        RecvStatus RecvMessage(InternalMessage& out, std::chrono::milliseconds timeout)
        {
            uint8_t header[4];
            const RecvStatus headerStatus = RecvExact(header, sizeof(header), timeout);
            if (headerStatus != RecvStatus::Frame)
                return headerStatus;
            const uint32_t length = (static_cast<uint32_t>(header[0]) << 24) |
                                    (static_cast<uint32_t>(header[1]) << 16) | (static_cast<uint32_t>(header[2]) << 8) |
                                    static_cast<uint32_t>(header[3]);
            std::vector<uint8_t> body(length);
            const RecvStatus bodyStatus = RecvExact(body.data(), body.size(), timeout);
            if (bodyStatus != RecvStatus::Frame)
                return bodyStatus;
            return DeserializeMessage(body.data(), body.size(), out) ? RecvStatus::Frame : RecvStatus::Closed;
        }

        // Waits for the host to drop the connection, discarding any frames it sends first.
        bool WaitForClose(std::chrono::milliseconds timeout)
        {
            const auto deadline = std::chrono::steady_clock::now() + timeout;
            while (std::chrono::steady_clock::now() < deadline)
            {
                char scratch[512];
                const RecvStatus status = RecvExact(scratch, 1, std::chrono::milliseconds(250));
                if (status == RecvStatus::Closed)
                    return true;
            }
            return false;
        }

        /// Completes the join handshake the way a real editor does; returns the assigned PeerID.
        PeerID Authenticate(const std::string& joinCode, const std::string& userName)
        {
            InternalMessage challenge;
            if (RecvMessage(challenge, std::chrono::seconds(5)) != RecvStatus::Frame ||
                challenge.type != InternalMessageType::AuthChallenge)
                return INVALID_PEER;
            InternalMessage connect;
            connect.type = InternalMessageType::PeerConnect;
            connect.peerInfo.userName = userName;
            connect.payload = ComputeCollabJoinProof(joinCode, challenge.payload, userName);
            if (!SendInternal(connect))
                return INVALID_PEER;
            InternalMessage accepted;
            if (RecvMessage(accepted, std::chrono::seconds(5)) != RecvStatus::Frame ||
                accepted.type != InternalMessageType::AuthAccepted)
                return INVALID_PEER;
            return accepted.sourcePeer;
        }

        TestSocketHandle socket = INVALID_TEST_SOCKET;
    };

    // Lets the host's accept/handler threads run, then drains its incoming queue once.
    void SettleAndUpdate(CollaborativeEditSession& session, std::chrono::milliseconds wait)
    {
        std::this_thread::sleep_for(wait);
        session.Update(0.1f);
    }
} // namespace

#ifdef _WIN32
static_assert(sizeof(CollaborativeSocketHandle) >= sizeof(void*),
              "collaborative WinSock handles must remain pointer-width");
#endif

// ============================================================================
// Serialization Tests
// ============================================================================

TEST(CollabEdit_SerializeDeserialize_Presence)
{
    InternalMessage msg;
    msg.type = InternalMessageType::Presence;
    msg.sourcePeer = 42;
    msg.nodeId = "";
    msg.timestamp = 12345;
    msg.peerInfo.id = 42;
    msg.peerInfo.userName = "Alice";
    msg.peerInfo.selectedNode = "Entity_7";
    msg.peerInfo.viewportCameraPos = {1.0f, 2.0f, 3.0f};
    msg.peerInfo.viewportCameraDir = {0.0f, 0.0f, -1.0f};

    auto data = SerializeMessage(msg);
    EXPECT_TRUE(data.size() > 0);

    InternalMessage decoded;
    bool ok = DeserializeMessage(data.data(), data.size(), decoded);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(decoded.type == InternalMessageType::Presence);
    EXPECT_EQ(decoded.sourcePeer, 42u);
    EXPECT_EQ(decoded.timestamp, 12345u);
    EXPECT_EQ(decoded.peerInfo.id, 42u);
    EXPECT_EQ(decoded.peerInfo.userName, "Alice");
    EXPECT_EQ(decoded.peerInfo.selectedNode, "Entity_7");
    EXPECT_TRUE(std::abs(decoded.peerInfo.viewportCameraPos.x - 1.0f) < 0.001f);
    EXPECT_TRUE(std::abs(decoded.peerInfo.viewportCameraPos.y - 2.0f) < 0.001f);
    EXPECT_TRUE(std::abs(decoded.peerInfo.viewportCameraPos.z - 3.0f) < 0.001f);
}

TEST(CollabEdit_SerializeDeserialize_EditBroadcast)
{
    InternalMessage msg;
    msg.type = InternalMessageType::EditBroadcast;
    msg.sourcePeer = 5;
    msg.nodeId = "Entity_42";
    msg.editMessage.type = EditMessageType::NodeModified;
    msg.editMessage.sourceEditor = 5;
    msg.editMessage.nodeId = "Entity_42";
    msg.editMessage.componentType = "Transform";
    msg.editMessage.propertyName = "position";
    msg.editMessage.newValue = "10.0, 5.0, 3.0";
    msg.editMessage.oldValue = "0.0, 0.0, 0.0";
    msg.editMessage.timestamp = 9999;

    auto data = SerializeMessage(msg);
    EXPECT_TRUE(data.size() > 0);

    InternalMessage decoded;
    bool ok = DeserializeMessage(data.data(), data.size(), decoded);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(decoded.editMessage.type == EditMessageType::NodeModified);
    EXPECT_EQ(decoded.editMessage.sourceEditor, 5u);
    EXPECT_EQ(decoded.editMessage.nodeId, "Entity_42");
    EXPECT_EQ(decoded.editMessage.componentType, "Transform");
    EXPECT_EQ(decoded.editMessage.propertyName, "position");
    EXPECT_EQ(decoded.editMessage.newValue, "10.0, 5.0, 3.0");
    EXPECT_EQ(decoded.editMessage.oldValue, "0.0, 0.0, 0.0");
    EXPECT_EQ(decoded.editMessage.timestamp, 9999u);
}

TEST(CollabEdit_SerializeDeserialize_LockRequest)
{
    InternalMessage msg;
    msg.type = InternalMessageType::LockRequest;
    msg.sourcePeer = 3;
    msg.nodeId = "Terrain_Root";
    msg.timestamp = 500;

    auto data = SerializeMessage(msg);
    InternalMessage decoded;
    bool ok = DeserializeMessage(data.data(), data.size(), decoded);
    EXPECT_TRUE(ok);
    EXPECT_TRUE(decoded.type == InternalMessageType::LockRequest);
    EXPECT_EQ(decoded.sourcePeer, 3u);
    EXPECT_EQ(decoded.nodeId, "Terrain_Root");
}

TEST(CollabEdit_NonEditSerialization_IsCanonical)
{
    InternalMessage baseline;
    baseline.type = InternalMessageType::LockRequest;
    baseline.sourcePeer = 3;
    baseline.nodeId = "Terrain_Root";

    InternalMessage withIrrelevantEdit = baseline;
    withIrrelevantEdit.editMessage.type = EditMessageType::ComponentRemoved;
    withIrrelevantEdit.editMessage.sourceEditor = 99;
    withIrrelevantEdit.editMessage.nodeId = "must-not-reach-wire";
    withIrrelevantEdit.editMessage.newValue = "must-not-reach-wire";

    EXPECT_TRUE(baseline.editMessage.type == EditMessageType::NodeModified);
    EXPECT_TRUE(SerializeMessage(baseline) == SerializeMessage(withIrrelevantEdit));
}

TEST(CollabEdit_EndpointPolicyRejectsWildcardExposure)
{
    using namespace Spark::Net;
    EXPECT_TRUE(ResolveNetworkEndpointPolicy("loopback").IsValid());
    EXPECT_TRUE(ResolveNetworkEndpointPolicy("192.168.1.20/24").IsValid());
    EXPECT_FALSE(ResolveNetworkEndpointPolicy("192.168.1.20").IsValid());
    EXPECT_FALSE(ResolveNetworkEndpointPolicy("").IsValid());
    EXPECT_FALSE(ResolveNetworkEndpointPolicy("typo").IsValid());
    EXPECT_FALSE(ResolveNetworkEndpointPolicy("all").IsValid());
    EXPECT_FALSE(ResolveNetworkEndpointPolicy("any").IsValid());
    EXPECT_FALSE(ResolveNetworkEndpointPolicy("public").IsValid());
    EXPECT_FALSE(ResolveNetworkEndpointPolicy("0.0.0.0").IsValid());
}

TEST(CollabEdit_DeserializeEmpty_ReturnsFalse)
{
    InternalMessage decoded;
    bool ok = DeserializeMessage(nullptr, 0, decoded);
    EXPECT_TRUE(!ok);
}

// ============================================================================
// Session Lifecycle Tests
// ============================================================================

TEST(CollabEdit_HostSession)
{
    CollaborativeEditSession session;
    EXPECT_TRUE(!session.IsConnected());
    EXPECT_TRUE(!session.IsHost());

    ASSERT_TRUE(session.Host(0, "TestHost")); // Port 0 = OS assigns
    EXPECT_TRUE(session.IsConnected());
    EXPECT_TRUE(session.IsHost());
    EXPECT_TRUE(session.GetLocalPeerID() != INVALID_PEER);
    EXPECT_TRUE(session.GetPort() != 0); // the OS-assigned port is reported, not the requested 0
    EXPECT_TRUE(IsValidCollabJoinCode(session.GetJoinCode()));

    auto peers = session.GetConnectedPeers();
    EXPECT_EQ(peers.size(), 1u);
    EXPECT_EQ(peers[0].userName, "TestHost");

    session.Disconnect();
    EXPECT_TRUE(!session.IsConnected());
    EXPECT_TRUE(session.GetJoinCode().empty());
}

TEST(CollabEdit_HostRejectsEmpty)
{
    CollaborativeEditSession session;
    bool hosted = session.Host(27099, "");
    EXPECT_TRUE(!hosted);
}

TEST(CollabEdit_DoubleHostRejects)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "Host1"));
    bool hosted2 = session.Host(0, "Host2");
    EXPECT_TRUE(!hosted2);
    session.Disconnect();
}

// ============================================================================
// Locking Tests
// ============================================================================

TEST(CollabEdit_LockAndRelease)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "LockTest"));

    bool locked = session.RequestLock("Entity_1");
    EXPECT_TRUE(locked);
    EXPECT_TRUE(session.IsLockedByLocal("Entity_1"));
    EXPECT_EQ(session.GetLockOwner("Entity_1"), session.GetLocalPeerID());

    auto locks = session.GetAllLocks();
    EXPECT_EQ(locks.size(), 1u);
    EXPECT_EQ(locks[0].nodeId, "Entity_1");

    session.ReleaseLock("Entity_1");
    EXPECT_TRUE(!session.IsLockedByLocal("Entity_1"));
    EXPECT_EQ(session.GetLockOwner("Entity_1"), INVALID_PEER);

    locks = session.GetAllLocks();
    EXPECT_EQ(locks.size(), 0u);

    session.Disconnect();
}

TEST(CollabEdit_DoubleLockSameNodeSucceeds)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "LockTest2"));

    EXPECT_TRUE(session.RequestLock("Node_A"));
    EXPECT_TRUE(session.RequestLock("Node_A"));

    session.Disconnect();
}

TEST(CollabEdit_LockCallbackFires)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "CallbackTest"));

    std::string lockedNode;
    PeerID lockOwner = INVALID_PEER;
    session.SetLockChangedCallback(
        [&](const std::string& nodeId, PeerID owner)
        {
            lockedNode = nodeId;
            lockOwner = owner;
        });

    session.RequestLock("Entity_99");
    EXPECT_EQ(lockedNode, "Entity_99");
    EXPECT_EQ(lockOwner, session.GetLocalPeerID());

    session.ReleaseLock("Entity_99");
    EXPECT_EQ(lockedNode, "Entity_99");
    EXPECT_EQ(lockOwner, INVALID_PEER);

    session.Disconnect();
}

// ============================================================================
// Edit Broadcasting Tests
// ============================================================================

TEST(CollabEdit_BroadcastEdit)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "EditTest"));

    EditMessage receivedEdit;
    bool editReceived = false;
    session.SetEditReceivedCallback(
        [&](const EditMessage& edit)
        {
            receivedEdit = edit;
            editReceived = true;
        });

    EditMessage edit;
    edit.type = EditMessageType::NodeModified;
    edit.sourceEditor = session.GetLocalPeerID();
    edit.nodeId = "Entity_42";
    edit.propertyName = "position";
    edit.newValue = "1,2,3";

    session.BroadcastEdit(edit);

    EXPECT_TRUE(editReceived);
    EXPECT_EQ(receivedEdit.nodeId, "Entity_42");
    EXPECT_EQ(receivedEdit.propertyName, "position");
    EXPECT_EQ(receivedEdit.newValue, "1,2,3");

    auto stats = session.GetStats();
    EXPECT_EQ(stats.editsBroadcast, 1u);

    session.Disconnect();
}

TEST(CollabEdit_RejectsInvalidEdit)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "EditTest2"));

    bool editReceived = false;
    session.SetEditReceivedCallback([&](const EditMessage&) { editReceived = true; });

    EditMessage edit;
    edit.type = EditMessageType::NodeModified;
    edit.sourceEditor = session.GetLocalPeerID();
    edit.nodeId = "";
    session.BroadcastEdit(edit);
    EXPECT_TRUE(!editReceived);

    edit.nodeId = "Entity_1";
    edit.sourceEditor = INVALID_PEER;
    session.BroadcastEdit(edit);
    EXPECT_TRUE(!editReceived);

    session.Disconnect();
}

// ============================================================================
// Peer Awareness Tests
// ============================================================================

TEST(CollabEdit_SetLocalSelection)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "SelectTest"));

    session.SetLocalSelection("Entity_7");

    auto peers = session.GetConnectedPeers();
    EXPECT_EQ(peers.size(), 1u);
    EXPECT_EQ(peers[0].selectedNode, "Entity_7");

    session.SetLocalSelection("");
    peers = session.GetConnectedPeers();
    EXPECT_EQ(peers[0].selectedNode, "");

    session.Disconnect();
}

TEST(CollabEdit_RejectsLongSelection)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "LongSelTest"));

    std::string longId(300, 'x');
    session.SetLocalSelection(longId);

    auto peers = session.GetConnectedPeers();
    EXPECT_TRUE(peers[0].selectedNode != longId);

    session.Disconnect();
}

TEST(CollabEdit_SetLocalViewportCamera)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "CamTest"));

    DirectX::XMFLOAT3 pos = {10.0f, 20.0f, 30.0f};
    DirectX::XMFLOAT3 dir = {0.0f, -1.0f, 0.0f};
    session.SetLocalViewportCamera(pos, dir);

    auto peers = session.GetConnectedPeers();
    EXPECT_TRUE(std::abs(peers[0].viewportCameraPos.x - 10.0f) < 0.001f);
    EXPECT_TRUE(std::abs(peers[0].viewportCameraPos.y - 20.0f) < 0.001f);
    EXPECT_TRUE(std::abs(peers[0].viewportCameraDir.y - (-1.0f)) < 0.001f);

    session.Disconnect();
}

// ============================================================================
// Session Statistics Tests
// ============================================================================

TEST(CollabEdit_Stats)
{
    CollaborativeEditSession session;
    ASSERT_TRUE(session.Host(0, "StatsTest"));

    session.Update(1.0f);

    auto stats = session.GetStats();
    EXPECT_EQ(stats.peerCount, 1u);
    EXPECT_EQ(stats.activeLocks, 0u);
    EXPECT_EQ(stats.editsBroadcast, 0u);
    EXPECT_EQ(stats.editsReceived, 0u);
    EXPECT_TRUE(stats.sessionDuration > 0.0f);

    auto status = session.Console_GetStatus();
    EXPECT_TRUE(status.find("Connected") != std::string::npos);
    EXPECT_TRUE(status.find("Host") != std::string::npos);

    session.Disconnect();
}

// ============================================================================
// TCP Host/Connect Integration Tests
// ============================================================================

TEST(CollabEdit_HostAndConnect)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "HostEditor"));
    const uint16_t testPort = host.GetPort();
    ASSERT_TRUE(testPort != 0);

    CollaborativeEditSession client;
    const bool connected = client.Connect("127.0.0.1", testPort, "ClientEditor", host.GetJoinCode());
    ASSERT_TRUE(connected);
    // The client adopts the PeerID the host assigned, so both sides agree on who it is.
    EXPECT_TRUE(client.GetLocalPeerID() != host.GetLocalPeerID());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    host.Update(0.1f);
    client.Update(0.1f);

    auto hostPeers = host.GetConnectedPeers();
    EXPECT_EQ(hostPeers.size(), 2u);
    const EditorPeer* joined = host.GetPeer(client.GetLocalPeerID());
    ASSERT_TRUE(joined != nullptr);
    EXPECT_EQ(joined->userName, "ClientEditor");

    client.Disconnect();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    host.Update(0.1f);
    host.Disconnect();
}

TEST(CollabEdit_ConnectToRefusedPort)
{
    CollaborativeEditSession client;
    RefusedPortReservation reservation;
    EXPECT_TRUE(reservation.Reserve());
    EXPECT_FALSE(client.Connect("127.0.0.1", reservation.port, "RefusedClient", kForeignJoinCode));
    EXPECT_FALSE(client.IsConnected());
}

// ============================================================================
// LiveEditBridge Tests
// ============================================================================

TEST(LiveEditBridge_DefaultState)
{
    LiveEditBridge bridge;
    EXPECT_TRUE(!bridge.IsConnected());
    EXPECT_EQ(bridge.GetEditsPushed(), 0u);
    EXPECT_TRUE(bridge.GetServerPort() == 0);
}

TEST(LiveEditBridge_ConnectToRefusedPort)
{
    LiveEditBridge bridge;
    bool connected = bridge.Connect("127.0.0.1", 59999, "TestEditor");
    EXPECT_TRUE(!connected);
    EXPECT_TRUE(!bridge.IsConnected());
}

TEST(CollabEdit_ProductionHostAndConnectRejectInvalidPolicyBeforeSocketUse)
{
    const auto invalidPolicy = Spark::Net::ResolveNetworkEndpointPolicy("0.0.0.0");
    ASSERT_FALSE(invalidPolicy.IsValid());

    CollaborativeEditSession host;
    EXPECT_FALSE(host.Host(0, "RejectedHost", invalidPolicy));
    EXPECT_FALSE(host.IsConnected());

    RefusedPortReservation listener;
    ASSERT_TRUE(listener.Reserve());
    ASSERT_TRUE(listener.Listen());
    CollaborativeEditSession client;
    EXPECT_FALSE(client.Connect("127.0.0.1", listener.port, "RejectedClient", kForeignJoinCode, invalidPolicy));
    EXPECT_FALSE(client.IsConnected());
}

TEST(LiveEditBridge_RejectsOutOfPolicyDestinationsBeforeSocketCreation)
{
    size_t socketFactoryCalls = 0;
    LiveEditBridge bridge(
        [&socketFactoryCalls]()
        {
            ++socketFactoryCalls;
            return INVALID_COLLAB_SOCKET;
        });
    const auto loopbackPolicy = Spark::Net::NetworkEndpointPolicy::Loopback();

    EXPECT_FALSE(bridge.Connect("203.0.113.9", 27015, "PolicyProbe", loopbackPolicy));
    EXPECT_FALSE(bridge.Connect("0.0.0.0", 27015, "PolicyProbe", loopbackPolicy));
    EXPECT_FALSE(bridge.Connect("::ffff:127.0.0.1", 27015, "PolicyProbe", loopbackPolicy));
    EXPECT_FALSE(bridge.Connect("127.000.0.1", 27015, "PolicyProbe", loopbackPolicy));
    EXPECT_EQ(socketFactoryCalls, static_cast<size_t>(0));

    // An admitted destination reaches the production socket boundary, proving
    // the preceding assertions did not merely fail for an unrelated argument.
    EXPECT_FALSE(bridge.Connect("127.0.0.1", 27015, "PolicyProbe", loopbackPolicy));
    EXPECT_EQ(socketFactoryCalls, static_cast<size_t>(1));
}

TEST(LiveEditBridge_PushWithoutConnect)
{
    LiveEditBridge bridge;
    EditMessage edit;
    edit.type = EditMessageType::NodeModified;
    edit.nodeId = "Entity_1";
    edit.sourceEditor = 1;

    bridge.PushEdit(edit);
    bridge.Update();
    EXPECT_EQ(bridge.GetEditsPushed(), 0u);
}

// ============================================================================
// Peer-session trust boundary (join-code authentication and resource bounds)
// ============================================================================

TEST(CollabEdit_JoinProofBindsCodeNonceAndName)
{
    const std::string code(kCollabJoinSecretBytes * 2, '1');
    const std::string nonce(kCollabChallengeNonceBytes, '\x5a');
    EXPECT_TRUE(IsValidCollabJoinCode(code));
    EXPECT_FALSE(IsValidCollabJoinCode(code.substr(1)));
    EXPECT_FALSE(IsValidCollabJoinCode(std::string(kCollabJoinSecretBytes * 2, 'A'))); // lowercase hex only
    EXPECT_FALSE(IsValidCollabJoinCode(std::string(kCollabJoinSecretBytes * 2, 'g')));

    const std::string proof = ComputeCollabJoinProof(code, nonce, "Alice");
    EXPECT_EQ(proof.size(), 64u);
    EXPECT_EQ(proof, ComputeCollabJoinProof(code, nonce, "Alice"));
    EXPECT_TRUE(proof != ComputeCollabJoinProof(kForeignJoinCode, nonce, "Alice"));
    EXPECT_TRUE(proof != ComputeCollabJoinProof(code, std::string(kCollabChallengeNonceBytes, '\x5b'), "Alice"));
    EXPECT_TRUE(proof != ComputeCollabJoinProof(code, nonce, "Mallory"));

    // A malformed code is refused before any socket work.
    CollaborativeEditSession client;
    EXPECT_FALSE(client.Connect("127.0.0.1", 1, "Client", "not-a-join-code"));
    EXPECT_FALSE(client.IsConnected());
}

TEST(CollabEdit_ConnectWithWrongJoinCodeIsRejected)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "Host"));

    CollaborativeEditSession intruder;
    EXPECT_FALSE(intruder.Connect("127.0.0.1", host.GetPort(), "Intruder", kForeignJoinCode));
    EXPECT_FALSE(intruder.IsConnected());

    SettleAndUpdate(host, std::chrono::milliseconds(200));
    EXPECT_EQ(host.GetConnectedPeers().size(), 1u);
}

TEST(CollabEdit_UnauthenticatedTrafficIsNeverQueuedOrRelayed)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "Host"));
    bool editSeen = false;
    host.SetEditReceivedCallback([&](const EditMessage&) { editSeen = true; });

    CollaborativeEditSession member;
    ASSERT_TRUE(member.Connect("127.0.0.1", host.GetPort(), "Member", host.GetJoinCode()));
    bool memberSawEdit = false;
    member.SetEditReceivedCallback([&](const EditMessage&) { memberSawEdit = true; });

    // Skip the handshake entirely and push an edit as the first frame.
    RawCollabPeer raw;
    ASSERT_TRUE(raw.Connect(host.GetPort()));
    InternalMessage challenge;
    ASSERT_TRUE(raw.RecvMessage(challenge, std::chrono::seconds(5)) == RawCollabPeer::RecvStatus::Frame);
    EXPECT_TRUE(challenge.type == InternalMessageType::AuthChallenge);
    EXPECT_EQ(challenge.payload.size(), kCollabChallengeNonceBytes);

    InternalMessage edit;
    edit.type = InternalMessageType::EditBroadcast;
    edit.nodeId = "Entity_1";
    edit.editMessage.nodeId = "Entity_1";
    edit.editMessage.sourceEditor = 99;
    ASSERT_TRUE(raw.SendInternal(edit));
    EXPECT_TRUE(raw.WaitForClose(std::chrono::seconds(3)));

    // A forged proof (wrong code) is refused the same way.
    RawCollabPeer forger;
    ASSERT_TRUE(forger.Connect(host.GetPort()));
    EXPECT_EQ(forger.Authenticate(kForeignJoinCode, "Forger"), INVALID_PEER);
    EXPECT_TRUE(forger.WaitForClose(std::chrono::seconds(3)));

    SettleAndUpdate(host, std::chrono::milliseconds(200));
    SettleAndUpdate(member, std::chrono::milliseconds(0));
    EXPECT_FALSE(editSeen);
    EXPECT_FALSE(memberSawEdit);
    EXPECT_EQ(host.GetStats().editsReceived, 0u);
    EXPECT_EQ(host.GetConnectedPeers().size(), 2u); // host + the one authenticated member
}

TEST(CollabEdit_StalledHandshakeIsDroppedAtDeadline)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "Host"));

    // Send a frame header promising more bytes, then stall: the handshake deadline
    // is absolute, so starting a frame must not extend it.
    RawCollabPeer staller;
    ASSERT_TRUE(staller.Connect(host.GetPort()));
    InternalMessage challenge;
    ASSERT_TRUE(staller.RecvMessage(challenge, std::chrono::seconds(5)) == RawCollabPeer::RecvStatus::Frame);
    ASSERT_TRUE(staller.SendHeader(100));
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(staller.WaitForClose(std::chrono::seconds(kCollabHandshakeTimeoutSeconds + 4)));
    EXPECT_TRUE(std::chrono::steady_clock::now() - start < std::chrono::seconds(kCollabFrameCompletionSeconds));
}

TEST(CollabEdit_OversizedFrameDisconnectsAuthenticatedPeer)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "Host"));
    bool disconnected = false;
    host.SetPeerDisconnectedCallback([&](PeerID) { disconnected = true; });

    RawCollabPeer peer;
    ASSERT_TRUE(peer.Connect(host.GetPort()));
    ASSERT_TRUE(peer.Authenticate(host.GetJoinCode(), "BigSender") != INVALID_PEER);

    // One byte over the cap is refused on the length prefix alone, before any
    // buffer for it is allocated.
    ASSERT_TRUE(peer.SendHeader(kCollabMaxFrameBytes + 1));
    EXPECT_TRUE(peer.WaitForClose(std::chrono::seconds(3)));

    SettleAndUpdate(host, std::chrono::milliseconds(200));
    EXPECT_TRUE(disconnected);
    EXPECT_EQ(host.GetConnectedPeers().size(), 1u);
}

TEST(CollabEdit_StalledFrameFromAuthenticatedPeerTimesOut)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "Host"));

    RawCollabPeer peer;
    ASSERT_TRUE(peer.Connect(host.GetPort()));
    ASSERT_TRUE(peer.Authenticate(host.GetJoinCode(), "Staller") != INVALID_PEER);

    // Start a frame and never finish it: the host must not pin the buffer and the
    // handler thread forever.
    ASSERT_TRUE(peer.SendHeader(100));
    const uint8_t partial[10] = {};
    ASSERT_TRUE(peer.SendRaw(partial, sizeof(partial)));
    EXPECT_TRUE(peer.WaitForClose(std::chrono::seconds(kCollabFrameCompletionSeconds + 5)));
}

TEST(CollabEdit_ConnectionCapRefusesExcessPeers)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "Host"));

    std::vector<std::unique_ptr<RawCollabPeer>> pending;
    for (size_t i = 0; i < kCollabMaxPeerConnections; ++i)
    {
        auto peer = std::make_unique<RawCollabPeer>();
        ASSERT_TRUE(peer->Connect(host.GetPort()));
        pending.push_back(std::move(peer));
        // Stay under the listen backlog: Windows refuses connects once it is full.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Every admitted connection is being serviced (it was sent a challenge)...
    InternalMessage challenge;
    ASSERT_TRUE(pending.back()->RecvMessage(challenge, std::chrono::seconds(3)) == RawCollabPeer::RecvStatus::Frame);
    EXPECT_TRUE(challenge.type == InternalMessageType::AuthChallenge);

    // ...and the next one is closed straight away, with no handler thread for it.
    RawCollabPeer excess;
    ASSERT_TRUE(excess.Connect(host.GetPort()));
    InternalMessage none;
    EXPECT_TRUE(excess.RecvMessage(none, std::chrono::seconds(3)) == RawCollabPeer::RecvStatus::Closed);
}

TEST(CollabEdit_IncomingQueueIsByteBounded)
{
    CollaborativeEditSession host;
    ASSERT_TRUE(host.Host(0, "Host"));
    bool disconnected = false;
    host.SetPeerDisconnectedCallback([&](PeerID) { disconnected = true; });

    RawCollabPeer peer;
    ASSERT_TRUE(peer.Connect(host.GetPort()));
    ASSERT_TRUE(peer.Authenticate(host.GetJoinCode(), "Flooder") != INVALID_PEER);

    // Queue more large (but individually legal) edits than the byte budget holds,
    // all before the host's main thread drains anything.
    constexpr uint32_t kEdits = 72;
    InternalMessage edit;
    edit.type = InternalMessageType::EditBroadcast;
    edit.nodeId = "Entity_1";
    edit.editMessage.nodeId = "Entity_1";
    edit.editMessage.newValue.assign(1000 * 1000, 'v');
    static_assert(size_t{kEdits} * 1000 * 1000 > kCollabMaxQueuedBytes, "the flood must exceed the byte budget");
    for (uint32_t i = 0; i < kEdits; ++i)
        ASSERT_TRUE(peer.SendInternal(edit));
    CloseTestSocket(peer.socket);
    peer.socket = INVALID_TEST_SOCKET;

    // The disconnect is queued after every edit, so seeing it in one Update proves
    // the whole flood was queued before that single drain.
    SettleAndUpdate(host, std::chrono::seconds(3));
    ASSERT_TRUE(disconnected);
    const uint32_t received = host.GetStats().editsReceived;
    EXPECT_TRUE(received > 0u);
    EXPECT_TRUE(received < kEdits);
    EXPECT_TRUE(size_t{received} * 1000 * 1000 <= kCollabMaxQueuedBytes);
}
