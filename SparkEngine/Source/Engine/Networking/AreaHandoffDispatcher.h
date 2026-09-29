#pragma once

#include "AreaHandoffParticipant.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <memory>

#ifdef ENABLE_NETWORKING
namespace Spark::Net
{
    /**
     * @brief Host-owned bridge from authenticated control threads to the game thread.
     *
     * Submit is safe from control-plane threads and allocates one bounded queue
     * record per request. Pump and participant callbacks run on the game thread;
     * the host owns this object and must clear the participant before module
     * unload. Stop is called before destroying the participant.
     */
    class AreaHandoffDispatcher
    {
      public:
        AreaHandoffDispatcher() = default;
        ~AreaHandoffDispatcher();
        AreaHandoffDispatcher(const AreaHandoffDispatcher&) = delete;
        AreaHandoffDispatcher& operator=(const AreaHandoffDispatcher&) = delete;

        void SetParticipant(IAreaHandoffParticipant* participant);
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
        static constexpr size_t kMaxPending = 1024;
    };
} // namespace Spark::Net
#endif
