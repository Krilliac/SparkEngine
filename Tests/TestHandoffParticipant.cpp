/** @file TestHandoffParticipant.cpp @brief Production handoff dispatcher tests. */
#include "TestFramework.h"

#ifdef ENABLE_NETWORKING

#include "Engine/Networking/AreaHandoffDispatcher.h"

#include <chrono>
#include <future>
#include <thread>
#include <vector>

namespace
{
    class RecordingParticipant final : public Spark::Net::IAreaHandoffParticipant
    {
      public:
        Spark::Net::HandoffResult Prepare(const Spark::Net::HandoffRequest&) override
        {
            return Record(Spark::Net::HandoffPhase::Prepare);
        }
        Spark::Net::HandoffResult Transfer(const Spark::Net::HandoffRequest&) override
        {
            return Record(Spark::Net::HandoffPhase::Transfer);
        }
        Spark::Net::HandoffResult Commit(const Spark::Net::HandoffRequest&) override
        {
            return Record(Spark::Net::HandoffPhase::Commit);
        }
        Spark::Net::HandoffResult Acknowledge(const Spark::Net::HandoffRequest&) override
        {
            return Record(Spark::Net::HandoffPhase::Acknowledge);
        }
        Spark::Net::HandoffResult Abort(const Spark::Net::HandoffRequest&) override
        {
            return Record(Spark::Net::HandoffPhase::Abort);
        }

        std::vector<Spark::Net::HandoffPhase> phases;
        std::thread::id callbackThread;

      private:
        Spark::Net::HandoffResult Record(Spark::Net::HandoffPhase phase)
        {
            callbackThread = std::this_thread::get_id();
            phases.push_back(phase);
            return Spark::Net::HandoffResult::Applied;
        }
    };

    Spark::Net::HandoffRequest MakeRequest()
    {
        Spark::Net::HandoffRequest request;
        request.sessionId = "tf-dispatch-session";
        request.epoch = 1;
        request.sourceArea = 7;
        request.targetArea = 8;
        return request;
    }

    void ExpectResult(Spark::Net::HandoffResult actual, Spark::Net::HandoffResult expected)
    {
        EXPECT_EQ(static_cast<int>(actual), static_cast<int>(expected));
    }
} // namespace

TEST(TF120_HandoffDispatcherRejectsUnboundAndStops)
{
    Spark::Net::AreaHandoffDispatcher dispatcher;
    const auto request = MakeRequest();
    ExpectResult(dispatcher.Submit(Spark::Net::HandoffPhase::Prepare, request), Spark::Net::HandoffResult::Unavailable);
    dispatcher.Stop();
    EXPECT_FALSE(dispatcher.IsReady());
}

TEST(TF120_HandoffDispatcherPumpsOnOwnerThread)
{
    Spark::Net::AreaHandoffDispatcher dispatcher;
    RecordingParticipant participant;
    dispatcher.SetParticipant(&participant);
    const auto request = MakeRequest();
    const std::thread::id callerThread = std::this_thread::get_id();
    auto submitted =
        std::async(std::launch::async, [&] { return dispatcher.Submit(Spark::Net::HandoffPhase::Prepare, request); });
    while (submitted.wait_for(std::chrono::milliseconds(1)) != std::future_status::ready)
    {
        dispatcher.Pump();
    }

    ExpectResult(submitted.get(), Spark::Net::HandoffResult::Applied);
    EXPECT_EQ(participant.phases.size(), size_t(1));
    EXPECT_TRUE(participant.callbackThread == callerThread);
}

TEST(TF120_HandoffDispatcherCancelsTimedOutRequest)
{
    Spark::Net::AreaHandoffDispatcher dispatcher;
    RecordingParticipant participant;
    dispatcher.SetParticipant(&participant);
    const auto request = MakeRequest();
    Spark::Net::HandoffResult result = Spark::Net::HandoffResult::Applied;
    std::thread submitter([&] { result = dispatcher.Submit(Spark::Net::HandoffPhase::Transfer, request); });
    submitter.join();

    ExpectResult(result, Spark::Net::HandoffResult::Unavailable);
    dispatcher.Pump();
    EXPECT_EQ(participant.phases.size(), size_t(0));
}

// A module cannot reach the host's EngineContext registry across its DLL boundary; it attaches and detaches its
// participant by publishing on the host EventBus, which the dispatcher follows until it is destroyed.
TEST(TF120_HandoffDispatcherFollowsParticipantEventsOnHostBus)
{
    Spark::EventBus bus;
    RecordingParticipant participant;
    {
        Spark::Net::AreaHandoffDispatcher dispatcher;
        dispatcher.BindParticipantEvents(bus);
        EXPECT_FALSE(dispatcher.IsReady());
        bus.Publish(Spark::Net::AreaHandoffParticipantChanged{&participant});
        EXPECT_TRUE(dispatcher.IsReady());
        bus.Publish(Spark::Net::AreaHandoffParticipantChanged{nullptr});
        EXPECT_FALSE(dispatcher.IsReady());
        bus.Publish(Spark::Net::AreaHandoffParticipantChanged{&participant});
        EXPECT_TRUE(dispatcher.IsReady());
    }
    // The destroyed dispatcher unsubscribed; a late module detach must not touch it.
    EXPECT_EQ(bus.SubscriberCount<Spark::Net::AreaHandoffParticipantChanged>(), size_t(0));
    bus.Publish(Spark::Net::AreaHandoffParticipantChanged{nullptr});
}

#endif // ENABLE_NETWORKING
