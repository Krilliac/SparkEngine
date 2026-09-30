#include "AreaHandoffDispatcher.h"

#ifdef ENABLE_NETWORKING
#include <chrono>
#include <cstdio>

namespace Spark::Net
{
    AreaHandoffDispatcher::~AreaHandoffDispatcher()
    {
        // Release pending submitters first; Stop() cannot throw.
        Stop();
        try
        {
            m_participantEvents.Unsubscribe();
        }
        catch (...)
        {
            // An exception must not escape a destructor. The engine logger may itself throw,
            // so report through the C stdio layer, which cannot.
            std::fputs("AreaHandoffDispatcher: participant event unsubscribe failed during teardown\n", stderr);
        }
    }

    void AreaHandoffDispatcher::SetParticipant(IAreaHandoffParticipant* participant)
    {
        std::lock_guard lock(m_mutex);
        m_participant = participant;
    }

    void AreaHandoffDispatcher::BindParticipantEvents(Spark::EventBus& bus)
    {
        m_participantEvents = bus.Subscribe<AreaHandoffParticipantChanged>(
            [this](const AreaHandoffParticipantChanged& changed) { SetParticipant(changed.participant); });
    }

    bool AreaHandoffDispatcher::IsReady() const noexcept
    {
        std::lock_guard lock(m_mutex);
        return !m_stopped && m_participant != nullptr;
    }

    HandoffResult AreaHandoffDispatcher::Submit(HandoffPhase phase, const HandoffRequest& request)
    {
        auto pending = std::make_shared<Pending>();
        pending->phase = phase;
        pending->request = request;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopped || m_participant == nullptr || m_pending.size() >= kMaxPending)
            {
                return HandoffResult::Unavailable;
            }
            m_pending.push_back(pending);
        }
        std::unique_lock lock(m_mutex);
        const bool completed =
            pending->completed.wait_for(lock, std::chrono::seconds(2), [&] { return pending->complete || m_stopped; });
        if (!completed || !pending->complete)
        {
            pending->canceled = true;
            return HandoffResult::Unavailable;
        }
        return pending->result;
    }

    void AreaHandoffDispatcher::Pump()
    {
        std::shared_ptr<Pending> pending;
        std::unique_lock lock(m_mutex);
        if (m_pending.empty() || m_stopped)
        {
            return;
        }
        pending = m_pending.front();
        m_pending.pop_front();
        HandoffResult result = HandoffResult::Unavailable;
        if (!pending->canceled && m_participant != nullptr)
        {
            switch (pending->phase)
            {
            case HandoffPhase::Prepare:
                result = m_participant->Prepare(pending->request);
                break;
            case HandoffPhase::Transfer:
                result = m_participant->Transfer(pending->request);
                break;
            case HandoffPhase::Commit:
                result = m_participant->Commit(pending->request);
                break;
            case HandoffPhase::Acknowledge:
                result = m_participant->Acknowledge(pending->request);
                break;
            case HandoffPhase::Abort:
                result = m_participant->Abort(pending->request);
                break;
            }
        }
        pending->result = result;
        pending->complete = true;
        lock.unlock();
        pending->completed.notify_one();
    }

    void AreaHandoffDispatcher::Stop() noexcept
    {
        std::lock_guard lock(m_mutex);
        m_stopped = true;
        for (const auto& pending : m_pending)
        {
            pending->result = HandoffResult::Unavailable;
            pending->complete = true;
            pending->completed.notify_one();
        }
        m_pending.clear();
    }
} // namespace Spark::Net
#endif
