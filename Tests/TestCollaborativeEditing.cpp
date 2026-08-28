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
#include <unistd.h>
#endif

#include "TestFramework.h"
#include "Communication/CollaborativeEditSession.h"
#include "Communication/LiveEditBridge.h"
#include "Engine/Networking/NetworkBindPolicy.h"
#include <cmath>
#include <thread>
#include <chrono>

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
        ~RefusedPortReservation()
        {
            CloseTestSocket(socket);
        }

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

        TestSocketHandle socket = INVALID_TEST_SOCKET;
        uint16_t port = 0;
    };
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
    EXPECT_TRUE(ResolveNetworkEndpointPolicy("192.168.1.20").IsValid());
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

    bool hosted = session.Host(0, "TestHost"); // Port 0 = OS assigns
    if (hosted)
    {
        EXPECT_TRUE(session.IsConnected());
        EXPECT_TRUE(session.IsHost());
        EXPECT_TRUE(session.GetLocalPeerID() != INVALID_PEER);

        auto peers = session.GetConnectedPeers();
        EXPECT_EQ(peers.size(), 1u);
        EXPECT_EQ(peers[0].userName, "TestHost");

        session.Disconnect();
        EXPECT_TRUE(!session.IsConnected());
    }
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
    bool hosted = session.Host(0, "Host1");
    if (hosted)
    {
        bool hosted2 = session.Host(0, "Host2");
        EXPECT_TRUE(!hosted2);
        session.Disconnect();
    }
}

// ============================================================================
// Locking Tests
// ============================================================================

TEST(CollabEdit_LockAndRelease)
{
    CollaborativeEditSession session;
    if (!session.Host(0, "LockTest"))
        return;

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
    if (!session.Host(0, "LockTest2"))
        return;

    EXPECT_TRUE(session.RequestLock("Node_A"));
    EXPECT_TRUE(session.RequestLock("Node_A"));

    session.Disconnect();
}

TEST(CollabEdit_LockCallbackFires)
{
    CollaborativeEditSession session;
    if (!session.Host(0, "CallbackTest"))
        return;

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
    if (!session.Host(0, "EditTest"))
        return;

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
    if (!session.Host(0, "EditTest2"))
        return;

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
    if (!session.Host(0, "SelectTest"))
        return;

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
    if (!session.Host(0, "LongSelTest"))
        return;

    std::string longId(300, 'x');
    session.SetLocalSelection(longId);

    auto peers = session.GetConnectedPeers();
    EXPECT_TRUE(peers[0].selectedNode != longId);

    session.Disconnect();
}

TEST(CollabEdit_SetLocalViewportCamera)
{
    CollaborativeEditSession session;
    if (!session.Host(0, "CamTest"))
        return;

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
    if (!session.Host(0, "StatsTest"))
        return;

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
    constexpr uint16_t testPort = 49199;

    CollaborativeEditSession host;
    bool hosted = host.Host(testPort, "HostEditor");
    if (!hosted)
        return;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    CollaborativeEditSession client;
    bool connected = client.Connect("127.0.0.1", testPort, "ClientEditor");
    EXPECT_TRUE(connected);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    host.Update(0.1f);
    client.Update(0.1f);

    auto hostPeers = host.GetConnectedPeers();
    EXPECT_TRUE(hostPeers.size() >= 2u);

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
    EXPECT_FALSE(client.Connect("127.0.0.1", reservation.port, "RefusedClient"));
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
