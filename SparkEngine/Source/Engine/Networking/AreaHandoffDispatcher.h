/**
 * @file AreaHandoffDispatcher.h
 * @brief Host-owned bridge from the authenticated area-control thread to the game-thread handoff participant.
 */
#pragma once

#include "AreaHandoffParticipant.h"
#include "Utils/EventBus.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>

#ifdef ENABLE_NETWORKING
namespace Spark::Net
{
    /**
     * @brief Host-owned bridge from authenticated control threads to the game thread.
     *
     * Submit is safe from control-plane threads and allocates one bounded queue
     * record per request. Pump and participant callbacks run on the game thread;
     * the host owns this object and outlives the participant it borrows. A module
     * attaches and detaches its participant by publishing AreaHandoffParticipantChanged
     * on the host EventBus passed to BindParticipantEvents. Stop is called before
     * destroying the participant.
     */
    class AreaHandoffDispatcher
    {
      public:
        AreaHandoffDispatcher() = default;
        ~AreaHandoffDispatcher();
        AreaHandoffDispatcher(const AreaHandoffDispatcher&) = delete;
        AreaHandoffDispatcher& operator=(const AreaHandoffDispatcher&) = delete;

        void SetParticipant(IAreaHandoffParticipant* participant);
        /// @brief Follow AreaHandoffParticipantChanged on @p bus; the bus must outlive this dispatcher.
        void BindParticipantEvents(Spark::EventBus& bus);
        [[nodiscard]] bool IsReady() const noexcept;
        [[nodiscard]] HandoffResult Submit(HandoffPhase phase, const HandoffRequest& request);
        void Pump();
        void Stop() noexcept;

      private:
        struct Pending
        {
            HandoffPhase phase;
            HandoffRequest request;
            HandoffResult result = HandoffResult::Unavailable;
            bool complete = false;
            bool canceled = false;
            std::condition_variable completed;
        };

        mutable std::mutex m_mutex;
        std::deque<std::shared_ptr<Pending>> m_pending;
        IAreaHandoffParticipant* m_participant = nullptr;
        bool m_stopped = false;
        Spark::SubscriptionHandle m_participantEvents;
        static constexpr size_t kMaxPending = 1024;
    };
} // namespace Spark::Net
#endif
