/**
 * @file TestReliableOrderedSequenceReal.cpp
 * @brief ReliableOrdered delivery must not depend on earlier Reliable traffic to the same peer.
 *
 * Regression: Reliable and ReliableOrdered messages were numbered from one per-peer sequence
 * counter, while the receiver's ordered stream expects its own contiguous numbering from 1. Any
 * earlier Reliable message (a client's ClientFinished, a server's reliable replication) pushed
 * the first ReliableOrdered message past the expected sequence, so it and every later ordered
 * message sat in the reorder buffer forever and no handler ever fired.
 *
 * The singleton NetworkManager can hold only one role at a time, so each test runs the
 * production sender and the production receiver in two phases over real loopback UDP: the
 * sender's datagrams are opened by a raw NET-100 peer (SecureTestPeer), and their inner
 * message bytes are replayed verbatim, re-sealed, into the production receiver. Nothing in
 * the replay depends on the inner wire layout, so the tests hold across wire revisions.
 */

#include "TestFramework.h"
#include "Fixtures/SecureTestPeer.h"
#include "Engine/Networking/NetworkManager.h"

#ifdef SPARK_TEST_HAS_IMGUI
#include "../GameModules/SparkGameMMO/Source/Chat/MMOChatSystem.h"
#include "Spark/IEngineContext.h"
#endif

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef ENABLE_NETWORKING

using namespace Spark::Net;
using namespace SparkTestFixtures;

namespace
{
    using InnerFrames = std::vector<std::vector<uint8_t>>;

    /// One message the production sender emits: its channel and a one-byte tag that identifies it.
    struct TaggedSend
    {
        ChannelType channel = ChannelType::Reliable;
        uint8_t tag = 0;
    };

    constexpr std::chrono::milliseconds kWindow(1500);

    uint16_t InnerType(std::span<const uint8_t> inner)
    {
        return inner.size() >= 6 ? static_cast<uint16_t>(inner[4] | (inner[5] << 8)) : uint16_t{0};
    }

    /// Open every datagram waiting on @p socket and keep the inner messages of type @p type.
    void CollectInner(LoopbackSocket& socket, SecureChannel* channel, MessageType type, InnerFrames& out)
    {
        while (auto datagram = socket.Receive())
        {
            auto inner = OpenFrame(channel, *datagram);
            if (inner && InnerType(*inner) == static_cast<uint16_t>(type))
            {
                out.push_back(std::move(*inner));
            }
        }
    }

    NetworkMessage Tagged(MessageType type, const TaggedSend& send)
    {
        NetworkMessage message;
        message.type = type;
        message.channel = send.channel;
        message.payload = {send.tag};
        return message;
    }

    /// The first byte of each delivered payload, in delivery order.
    struct Recorder
    {
        std::vector<uint8_t> tags;

        NetworkManager::MessageHandler Handler()
        {
            return [this](const NetworkMessage& message)
            {
                if (!message.payload.empty())
                {
                    tags.push_back(message.payload.front());
                }
            };
        }
    };

    bool StartLoopbackServer(NetworkManager& manager)
    {
        manager.Shutdown();
        return manager.Initialize() && manager.StartServer(0, 4, NetworkEndpointPolicy::Loopback());
    }

    /// Connect the singleton as a client of @p fake, which admits it as client @p id.
    bool ConnectClient(NetworkManager& client, SecureRawServer& fake, ClientID id)
    {
        if (!client.Connect("127.0.0.1", fake.Port(), "OrderedPeer") || !fake.AwaitConnect(client))
        {
            return false;
        }
        if (fake.Accept(id).empty())
        {
            return false;
        }
        return PumpUntil(client, [&] { return client.GetConnectionState() == ConnectionState::Connected; }, kWindow);
    }

    /// Server phase: the production server sends @p sends to one raw client; returns the inner frames.
    InnerFrames CaptureServerToClient(const std::vector<TaggedSend>& sends)
    {
        auto& server = NetworkManager::GetInstance();
        InnerFrames frames;
        if (!StartLoopbackServer(server))
        {
            return frames;
        }
        SecureRawClient peer;
        if (peer.Connect(server, "OrderedReceiver"))
        {
            for (const TaggedSend& send : sends)
            {
                server.SendToClient(peer.Id(), Tagged(MessageType::UserDefined, send));
            }
            PumpUntil(
                server,
                [&]
                {
                    CollectInner(peer.Socket(), peer.Channel(), MessageType::UserDefined, frames);
                    return frames.size() >= sends.size();
                },
                kWindow);
        }
        server.Shutdown();
        return frames;
    }

    /// Client phase: replay @p frames (by index, in @p order) from a raw server into a fresh client.
    std::vector<uint8_t> ReplayIntoClient(const InnerFrames& frames, const std::vector<size_t>& order,
                                          size_t expectedDeliveries)
    {
        auto& client = NetworkManager::GetInstance();
        client.Shutdown();
        Recorder recorder;
        SecureRawServer fake;
        if (!client.Initialize() || !ConnectClient(client, fake, 7))
        {
            client.Shutdown();
            return recorder.tags;
        }
        client.RegisterHandler(MessageType::UserDefined, recorder.Handler());
        for (const size_t index : order)
        {
            fake.SendSealed(frames[index]);
        }
        PumpUntil(client, [&] { return recorder.tags.size() >= expectedDeliveries; }, kWindow);
        // Settle a little longer so a duplicate delivery would be observed.
        PumpUntil(client, [] { return false; }, std::chrono::milliseconds(60));
        client.Shutdown();
        return recorder.tags;
    }

    std::vector<size_t> InSendOrder(size_t count)
    {
        std::vector<size_t> order(count);
        for (size_t i = 0; i < count; ++i)
        {
            order[i] = i;
        }
        return order;
    }

    /// The delivered tags that belong to the ordered stream (tags >= 0x80), in delivery order.
    std::vector<uint8_t> OrderedTags(const std::vector<uint8_t>& delivered)
    {
        std::vector<uint8_t> ordered;
        for (const uint8_t tag : delivered)
        {
            if (tag >= 0x80)
            {
                ordered.push_back(tag);
            }
        }
        return ordered;
    }

    size_t CountTag(const std::vector<uint8_t>& delivered, uint8_t tag)
    {
        size_t count = 0;
        for (const uint8_t value : delivered)
        {
            count += value == tag ? 1u : 0u;
        }
        return count;
    }
} // namespace

// Reliable tags are < 0x80, ordered tags are >= 0x80.

TEST(ReliableOrderedSeq_ServerToClient_OrderedAfterReliableIsDelivered)
{
    const std::vector<TaggedSend> sends = {{ChannelType::Reliable, 0x01},
                                           {ChannelType::Reliable, 0x02},
                                           {ChannelType::ReliableOrdered, 0x80},
                                           {ChannelType::ReliableOrdered, 0x81},
                                           {ChannelType::ReliableOrdered, 0x82}};
    const InnerFrames frames = CaptureServerToClient(sends);
    ASSERT_EQ(frames.size(), sends.size());

    const auto delivered = ReplayIntoClient(frames, InSendOrder(frames.size()), sends.size());
    EXPECT_EQ(delivered.size(), sends.size());
    EXPECT_EQ(CountTag(delivered, 0x01), static_cast<size_t>(1));
    EXPECT_EQ(CountTag(delivered, 0x02), static_cast<size_t>(1));
    EXPECT_TRUE(OrderedTags(delivered) == (std::vector<uint8_t>{0x80, 0x81, 0x82}));
}

TEST(ReliableOrderedSeq_ClientToServer_OrderedAfterReliableIsDelivered)
{
    // Client phase: the production client has already sent its Reliable ClientFinished; add one
    // more Reliable message, then the ordered stream.
    const std::vector<TaggedSend> sends = {{ChannelType::Reliable, 0x11},
                                           {ChannelType::ReliableOrdered, 0x90},
                                           {ChannelType::ReliableOrdered, 0x91},
                                           {ChannelType::ReliableOrdered, 0x92}};
    InnerFrames frames;
    {
        auto& client = NetworkManager::GetInstance();
        client.Shutdown();
        SecureRawServer fake;
        ASSERT_TRUE(client.Initialize());
        ASSERT_TRUE(ConnectClient(client, fake, 7));
        for (const TaggedSend& send : sends)
        {
            client.SendMessage(Tagged(MessageType::UserDefined, send));
        }
        PumpUntil(
            client,
            [&]
            {
                CollectInner(fake.Socket(), fake.Channel(), MessageType::UserDefined, frames);
                return frames.size() >= sends.size();
            },
            kWindow);
        client.Shutdown();
    }
    ASSERT_EQ(frames.size(), sends.size());

    // Server phase: replay them from a raw client into the production server.
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    Recorder recorder;
    server.RegisterHandler(MessageType::UserDefined, recorder.Handler());
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "OrderedSender"));
    for (const auto& frame : frames)
    {
        peer.SendSealed(frame);
    }
    PumpUntil(server, [&] { return recorder.tags.size() >= sends.size(); }, kWindow);
    server.Shutdown();

    EXPECT_EQ(recorder.tags.size(), sends.size());
    EXPECT_EQ(CountTag(recorder.tags, 0x11), static_cast<size_t>(1));
    EXPECT_TRUE(OrderedTags(recorder.tags) == (std::vector<uint8_t>{0x90, 0x91, 0x92}));
}

TEST(ReliableOrderedSeq_InterleavedStreamsDeliverInOrderOnce)
{
    const std::vector<TaggedSend> sends = {
        {ChannelType::Reliable, 0x21},        {ChannelType::ReliableOrdered, 0xA0},
        {ChannelType::Reliable, 0x22},        {ChannelType::ReliableOrdered, 0xA1},
        {ChannelType::Reliable, 0x23},        {ChannelType::ReliableOrdered, 0xA2},
        {ChannelType::ReliableOrdered, 0xA3},
    };
    const InnerFrames frames = CaptureServerToClient(sends);
    ASSERT_EQ(frames.size(), sends.size());

    const auto delivered = ReplayIntoClient(frames, InSendOrder(frames.size()), sends.size());
    EXPECT_EQ(delivered.size(), sends.size());
    for (const uint8_t tag : {uint8_t{0x21}, uint8_t{0x22}, uint8_t{0x23}})
    {
        EXPECT_EQ(CountTag(delivered, tag), static_cast<size_t>(1));
    }
    EXPECT_TRUE(OrderedTags(delivered) == (std::vector<uint8_t>{0xA0, 0xA1, 0xA2, 0xA3}));
}

TEST(ReliableOrderedSeq_LostReorderedAndDuplicatedOrderedFrames)
{
    // Frames: [0]=R 0x31, [1]=O 0xB0, [2]=O 0xB1, [3]=O 0xB2, [4]=O 0xB3.
    const std::vector<TaggedSend> sends = {{ChannelType::Reliable, 0x31},
                                           {ChannelType::ReliableOrdered, 0xB0},
                                           {ChannelType::ReliableOrdered, 0xB1},
                                           {ChannelType::ReliableOrdered, 0xB2},
                                           {ChannelType::ReliableOrdered, 0xB3}};
    const InnerFrames frames = CaptureServerToClient(sends);
    ASSERT_EQ(frames.size(), sends.size());

    auto& client = NetworkManager::GetInstance();
    client.Shutdown();
    Recorder recorder;
    SecureRawServer fake;
    ASSERT_TRUE(client.Initialize());
    ASSERT_TRUE(ConnectClient(client, fake, 7));
    client.RegisterHandler(MessageType::UserDefined, recorder.Handler());

    // 0xB1 is "lost" for now; the rest arrive reordered, with duplicates of 0xB0 and 0xB2.
    for (const size_t index : {size_t{3}, size_t{0}, size_t{1}, size_t{4}, size_t{1}, size_t{3}})
    {
        fake.SendSealed(frames[index]);
    }
    PumpUntil(client, [&] { return recorder.tags.size() >= 2; }, kWindow);
    PumpUntil(client, [] { return false; }, std::chrono::milliseconds(60));
    EXPECT_EQ(CountTag(recorder.tags, 0x31), static_cast<size_t>(1));
    EXPECT_TRUE(OrderedTags(recorder.tags) == (std::vector<uint8_t>{0xB0})); // held behind the gap

    // The "retransmission" of 0xB1 fills the gap and releases the buffered successors once each.
    fake.SendSealed(frames[2]);
    PumpUntil(client, [&] { return recorder.tags.size() >= sends.size(); }, kWindow);
    PumpUntil(client, [] { return false; }, std::chrono::milliseconds(60));
    client.Shutdown();

    EXPECT_EQ(recorder.tags.size(), sends.size());
    EXPECT_TRUE(OrderedTags(recorder.tags) == (std::vector<uint8_t>{0xB0, 0xB1, 0xB2, 0xB3}));
}

TEST(ReliableOrderedSeq_ReconnectRestartsBothStreams)
{
    // Server side: one peer's session, a kick, then a fresh session for a reconnecting peer.
    // Each session's streams must start over so its ordered frames stand alone.
    InnerFrames firstSession;
    InnerFrames secondSession;
    {
        auto& server = NetworkManager::GetInstance();
        ASSERT_TRUE(StartLoopbackServer(server));
        const std::vector<TaggedSend> first = {
            {ChannelType::Reliable, 0x41}, {ChannelType::ReliableOrdered, 0xC0}, {ChannelType::ReliableOrdered, 0xC1}};
        const std::vector<TaggedSend> second = {
            {ChannelType::Reliable, 0x42}, {ChannelType::ReliableOrdered, 0xD0}, {ChannelType::ReliableOrdered, 0xD1}};
        {
            SecureRawClient peer;
            ASSERT_TRUE(peer.Connect(server, "Reconnector"));
            for (const TaggedSend& send : first)
            {
                server.SendToClient(peer.Id(), Tagged(MessageType::UserDefined, send));
            }
            PumpUntil(
                server,
                [&]
                {
                    CollectInner(peer.Socket(), peer.Channel(), MessageType::UserDefined, firstSession);
                    return firstSession.size() >= first.size();
                },
                kWindow);
            server.KickClient(peer.Id(), "reconnect test");
            PumpUntil(server, [&] { return server.GetClients().empty(); }, kWindow);
        }
        SecureRawClient again;
        ASSERT_TRUE(again.Connect(server, "Reconnector"));
        for (const TaggedSend& send : second)
        {
            server.SendToClient(again.Id(), Tagged(MessageType::UserDefined, send));
        }
        PumpUntil(
            server,
            [&]
            {
                CollectInner(again.Socket(), again.Channel(), MessageType::UserDefined, secondSession);
                return secondSession.size() >= second.size();
            },
            kWindow);
        server.Shutdown();
    }
    ASSERT_EQ(firstSession.size(), static_cast<size_t>(3));
    ASSERT_EQ(secondSession.size(), static_cast<size_t>(3));

    // Client side: one session, Disconnect, then a new session against a new server. The
    // client's receive streams must restart, or the second session's ordered frames stall.
    auto& client = NetworkManager::GetInstance();
    client.Shutdown();
    ASSERT_TRUE(client.Initialize());
    Recorder recorder;
    {
        SecureRawServer fake;
        ASSERT_TRUE(ConnectClient(client, fake, 7));
        client.RegisterHandler(MessageType::UserDefined, recorder.Handler());
        for (const auto& frame : firstSession)
        {
            fake.SendSealed(frame);
        }
        PumpUntil(client, [&] { return recorder.tags.size() >= 3; }, kWindow);
        client.Disconnect();
    }
    {
        SecureRawServer fake;
        ASSERT_TRUE(ConnectClient(client, fake, 8));
        client.RegisterHandler(MessageType::UserDefined, recorder.Handler());
        for (const auto& frame : secondSession)
        {
            fake.SendSealed(frame);
        }
        PumpUntil(client, [&] { return recorder.tags.size() >= 6; }, kWindow);
        PumpUntil(client, [] { return false; }, std::chrono::milliseconds(60));
    }
    client.Shutdown();

    EXPECT_EQ(recorder.tags.size(), static_cast<size_t>(6));
    EXPECT_EQ(CountTag(recorder.tags, 0x41), static_cast<size_t>(1));
    EXPECT_EQ(CountTag(recorder.tags, 0x42), static_cast<size_t>(1));
    EXPECT_TRUE(OrderedTags(recorder.tags) == (std::vector<uint8_t>{0xC0, 0xC1, 0xD0, 0xD1}));
}

#ifdef SPARK_TEST_HAS_IMGUI

namespace
{
    /// Engine context that hands MMOChatSystem the NetworkManager singleton.
    class NetworkOnlyContext : public Spark::IEngineContext
    {
      public:
        GraphicsEngine* GetGraphics() override { return nullptr; }
        const GraphicsEngine* GetGraphics() const override { return nullptr; }
        InputManager* GetInput() override { return nullptr; }
        const InputManager* GetInput() const override { return nullptr; }
        Timer* GetTimer() override { return nullptr; }
        const Timer* GetTimer() const override { return nullptr; }
        Spark::EventBus* GetEventBus() override { return nullptr; }
        const Spark::EventBus* GetEventBus() const override { return nullptr; }
        ::AudioEngine* GetAudio() override { return nullptr; }
        const ::AudioEngine* GetAudio() const override { return nullptr; }
        PhysicsSystem* GetPhysics() override { return nullptr; }
        const PhysicsSystem* GetPhysics() const override { return nullptr; }
        uint32_t GetEngineVersion() const override { return 0; }
        uint32_t GetSDKVersion() const override { return 0; }
        NetworkManager* GetNetwork() override { return &NetworkManager::GetInstance(); }
        const NetworkManager* GetNetwork() const override { return &NetworkManager::GetInstance(); }
    };

    constexpr auto kMMOChatType = static_cast<MessageType>(static_cast<uint16_t>(MessageType::UserDefined) + 1u);

    bool HistoryContains(const MMO::MMOChatSystem& chat, const std::string& text)
    {
        for (const auto& entry : chat.GetHistory())
        {
            if (entry.text == text)
            {
                return true;
            }
        }
        return false;
    }
} // namespace

TEST(ReliableOrderedSeq_MMOChatFromClientReachesServer)
{
    NetworkOnlyContext context;
    const std::string text = "ordered hello";

    // Client phase: a connected MMO client (its ClientFinished already went out Reliable) sends
    // one more Reliable message and then area chat, which MMOChatSystem puts on ReliableOrdered.
    InnerFrames chatFrames;
    {
        auto& client = NetworkManager::GetInstance();
        client.Shutdown();
        SecureRawServer fake;
        ASSERT_TRUE(client.Initialize());
        ASSERT_TRUE(ConnectClient(client, fake, 7));
        MMO::MMOChatSystem chat;
        ASSERT_TRUE(chat.Initialize(&context));
        client.SendMessage(Tagged(MessageType::UserDefined, {ChannelType::Reliable, 0x51}));
        chat.SendMessage(MMO::ChatChannel::Area, text);
        chat.SendMessage(MMO::ChatChannel::Area, text + " again");
        PumpUntil(
            client,
            [&]
            {
                CollectInner(fake.Socket(), fake.Channel(), kMMOChatType, chatFrames);
                return chatFrames.size() >= 2;
            },
            kWindow);
        chat.Shutdown();
        client.Shutdown();
    }
    ASSERT_EQ(chatFrames.size(), static_cast<size_t>(2));

    // Server phase: the MMO server's chat handler must fire for both messages, in order.
    auto& server = NetworkManager::GetInstance();
    ASSERT_TRUE(StartLoopbackServer(server));
    MMO::MMOChatSystem serverChat;
    ASSERT_TRUE(serverChat.Initialize(&context));
    SecureRawClient peer;
    ASSERT_TRUE(peer.Connect(server, "Chatter"));
    for (const auto& frame : chatFrames)
    {
        peer.SendSealed(frame);
    }
    // The network handler only queues; the game thread's Update records the history.
    PumpUntil(
        server,
        [&]
        {
            serverChat.Update(0.016f);
            return HistoryContains(serverChat, text + " again");
        },
        kWindow);

    EXPECT_TRUE(HistoryContains(serverChat, text));
    EXPECT_TRUE(HistoryContains(serverChat, text + " again"));
    const auto& history = serverChat.GetHistory();
    ASSERT_TRUE(history.size() >= 2u);
    EXPECT_EQ(history[history.size() - 2].text, text);
    EXPECT_EQ(history.back().text, text + " again");

    serverChat.Shutdown();
    server.Shutdown();
}

#endif // SPARK_TEST_HAS_IMGUI

#endif // ENABLE_NETWORKING
