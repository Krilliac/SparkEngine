/**
 * @file Test_editor_collab_lock_protocol.cpp
 * @brief Regression test: the authoritative lock-arbitration reply messages
 *        (LockGranted / LockDenied) survive the CollaborativeEditSession wire
 *        protocol intact.
 *
 * The P1 fix makes the host the single lock arbiter: clients send a LockRequest
 * and the host replies with an authoritative LockGranted (broadcast to all peers
 * so every view converges on the new owner) or LockDenied (to the loser). Those
 * two message types are new. If they did not round-trip through
 * SerializeMessage()/DeserializeMessage(), a granted/denied decision would be
 * dropped or misread on the wire and two peers could still diverge — exactly the
 * bug this fix closes. These tests exercise the free serialization functions
 * directly (no sockets), so they are deterministic and network-free.
 */

#include "../TestFramework.h"

#include "../../SparkEditor/Source/Communication/CollaborativeEditSession.h"

#include <string>
#include <vector>

using namespace SparkEditor;

namespace
{
    InternalMessage RoundTrip(const InternalMessage& in, bool& ok)
    {
        const std::vector<uint8_t> bytes = SerializeMessage(in);
        InternalMessage out;
        ok = DeserializeMessage(bytes.data(), bytes.size(), out);
        return out;
    }
} // namespace

TEST(Editor_Collab_LockGranted_SerializesRoundTrip)
{
    InternalMessage msg;
    msg.type = InternalMessageType::LockGranted;
    msg.sourcePeer = 42; // the peer the grant is about
    msg.nodeId = "Entity_7";
    msg.timestamp = 123456;

    bool ok = false;
    const InternalMessage out = RoundTrip(msg, ok);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(out.type == InternalMessageType::LockGranted);
    EXPECT_EQ(out.sourcePeer, static_cast<PeerID>(42));
    EXPECT_EQ(out.nodeId, std::string("Entity_7"));
    EXPECT_EQ(out.timestamp, static_cast<uint64_t>(123456));
}

TEST(Editor_Collab_LockDenied_SerializesRoundTrip)
{
    InternalMessage msg;
    msg.type = InternalMessageType::LockDenied;
    msg.sourcePeer = 5; // the denied requester
    msg.nodeId = "Node_A";
    msg.timestamp = 99;

    bool ok = false;
    const InternalMessage out = RoundTrip(msg, ok);

    EXPECT_TRUE(ok);
    EXPECT_TRUE(out.type == InternalMessageType::LockDenied);
    EXPECT_EQ(out.sourcePeer, static_cast<PeerID>(5));
    EXPECT_EQ(out.nodeId, std::string("Node_A"));
    EXPECT_EQ(out.timestamp, static_cast<uint64_t>(99));
}

TEST(Editor_Collab_LockReplyTypes_AreDistinct)
{
    // The grant/deny replies must be distinct wire values from the request and
    // from each other, or the host's arbitration decision would be ambiguous.
    EXPECT_TRUE(InternalMessageType::LockGranted != InternalMessageType::LockDenied);
    EXPECT_TRUE(InternalMessageType::LockGranted != InternalMessageType::LockRequest);
    EXPECT_TRUE(InternalMessageType::LockDenied != InternalMessageType::LockRelease);
}
