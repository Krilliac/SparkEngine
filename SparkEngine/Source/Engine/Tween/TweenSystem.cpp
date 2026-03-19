/**
 * @file TweenSystem.cpp
 * @brief Advanced tween system implementation
 */

#include "TweenSystem.h"

#include <algorithm>
#include <cmath>

namespace Spark
{

    // ============================================================================
    // Ease functions
    // ============================================================================

    float ApplyEase(EaseType ease, float t)
    {
        switch (ease)
        {
        case EaseType::Linear:
            return t;
        case EaseType::EaseInQuad:
            return t * t;
        case EaseType::EaseOutQuad:
            return t * (2.0f - t);
        case EaseType::EaseInOutQuad:
            return (t < 0.5f) ? (2.0f * t * t) : (-1.0f + (4.0f - 2.0f * t) * t);
        case EaseType::EaseInCubic:
            return t * t * t;
        case EaseType::EaseOutCubic:
        {
            float f = t - 1.0f;
            return f * f * f + 1.0f;
        }
        case EaseType::EaseInOutCubic:
            return (t < 0.5f) ? (4.0f * t * t * t) : ((t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f);
        case EaseType::EaseInElastic:
        {
            if (t <= 0.0f)
                return 0.0f;
            if (t >= 1.0f)
                return 1.0f;
            return -std::pow(2.0f, 10.0f * (t - 1.0f)) * std::sin((t - 1.1f) * 5.0f * 3.14159265f);
        }
        case EaseType::EaseOutElastic:
        {
            if (t <= 0.0f)
                return 0.0f;
            if (t >= 1.0f)
                return 1.0f;
            return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.1f) * 5.0f * 3.14159265f) + 1.0f;
        }
        case EaseType::EaseOutBounce:
        {
            if (t < 1.0f / 2.75f)
                return 7.5625f * t * t;
            else if (t < 2.0f / 2.75f)
            {
                float f = t - 1.5f / 2.75f;
                return 7.5625f * f * f + 0.75f;
            }
            else if (t < 2.5f / 2.75f)
            {
                float f = t - 2.25f / 2.75f;
                return 7.5625f * f * f + 0.9375f;
            }
            else
            {
                float f = t - 2.625f / 2.75f;
                return 7.5625f * f * f + 0.984375f;
            }
        }
        default:
            return t;
        }
    }

    // ============================================================================
    // TweenInstance
    // ============================================================================

    TweenInstance::TweenInstance(float duration, UpdateCallback onUpdate, EaseType ease)
        : m_duration(duration), m_ease(ease), m_onUpdate(std::move(onUpdate))
    {
    }

    TweenInstance& TweenInstance::OnComplete(CompleteCallback callback)
    {
        m_onComplete = std::move(callback);
        return *this;
    }

    TweenInstance& TweenInstance::SetLoop(LoopMode mode, int32_t count)
    {
        m_loopMode = mode;
        m_loopCount = count;
        return *this;
    }

    TweenInstance& TweenInstance::SetDelay(float delay)
    {
        m_delay = delay;
        return *this;
    }

    TweenInstance& TweenInstance::SetPlayRate(float rate)
    {
        m_playRate = rate;
        return *this;
    }

    void TweenInstance::Pause()
    {
        if (m_state == TweenState::Running)
            m_state = TweenState::Paused;
    }

    void TweenInstance::Resume()
    {
        if (m_state == TweenState::Paused)
            m_state = TweenState::Running;
    }

    void TweenInstance::Cancel()
    {
        m_state = TweenState::Completed;
    }

    void TweenInstance::Restart()
    {
        m_elapsed = 0.0f;
        m_currentLoop = 0;
        m_state = TweenState::Pending;
    }

    bool TweenInstance::Update(float dt)
    {
        if (m_state == TweenState::Completed || m_state == TweenState::Paused)
            return m_state == TweenState::Completed;

        if (m_state == TweenState::Pending)
        {
            m_delay -= dt * m_playRate;
            if (m_delay > 0.0f)
                return false;
            m_state = TweenState::Running;
            dt = -m_delay;
        }

        m_elapsed += dt * m_playRate;

        float rawT = (m_duration > 0.0f) ? (m_elapsed / m_duration) : 1.0f;
        rawT = std::clamp(rawT, 0.0f, 1.0f);

        float easedT = ApplyEase(m_ease, rawT);

        if (m_onUpdate)
            m_onUpdate(easedT);

        if (rawT >= 1.0f)
        {
            if (m_loopMode == LoopMode::None)
            {
                m_state = TweenState::Completed;
                if (m_onComplete)
                    m_onComplete();
                return true;
            }

            m_currentLoop++;
            if (m_loopCount > 0 && m_currentLoop >= m_loopCount)
            {
                m_state = TweenState::Completed;
                if (m_onComplete)
                    m_onComplete();
                return true;
            }

            m_elapsed = 0.0f;
            if (m_loopMode == LoopMode::PingPong)
                m_playRate = -m_playRate;
        }

        return false;
    }

    // ============================================================================
    // TweenSystem
    // ============================================================================

    TweenSystem& TweenSystem::GetInstance()
    {
        static TweenSystem instance;
        return instance;
    }

    bool TweenSystem::Initialize()
    {
        m_tweens.clear();
        m_nextHandle = 1;
        return true;
    }

    TweenHandle TweenSystem::Create(float duration, TweenInstance::UpdateCallback onUpdate, EaseType ease)
    {
        TweenHandle handle = m_nextHandle++;
        m_tweens[handle] = TweenInstance(duration, std::move(onUpdate), ease);
        return handle;
    }

    TweenInstance* TweenSystem::Get(TweenHandle handle)
    {
        auto it = m_tweens.find(handle);
        return (it != m_tweens.end()) ? &it->second : nullptr;
    }

    void TweenSystem::Cancel(TweenHandle handle)
    {
        auto it = m_tweens.find(handle);
        if (it != m_tweens.end())
            it->second.Cancel();
    }

    void TweenSystem::CancelAll()
    {
        for (auto& [h, tween] : m_tweens)
            tween.Cancel();
        m_tweens.clear();
    }

    void TweenSystem::Pause(TweenHandle handle)
    {
        auto it = m_tweens.find(handle);
        if (it != m_tweens.end())
            it->second.Pause();
    }

    void TweenSystem::Resume(TweenHandle handle)
    {
        auto it = m_tweens.find(handle);
        if (it != m_tweens.end())
            it->second.Resume();
    }

    void TweenSystem::Update(float deltaTime)
    {
        auto it = m_tweens.begin();
        while (it != m_tweens.end())
        {
            if (it->second.Update(deltaTime))
            {
                it = m_tweens.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    TweenHandle TweenSystem::TweenFloat(float& target, float from, float to, float duration, EaseType ease)
    {
        float* ptr = &target;
        return Create(duration, [ptr, from, to](float t) { *ptr = from + (to - from) * t; }, ease);
    }

    TweenHandle TweenSystem::TweenDelay(float delay, TweenInstance::CompleteCallback onComplete)
    {
        auto handle = Create(0.01f, nullptr, EaseType::Linear);
        if (auto* tween = Get(handle))
        {
            tween->SetDelay(delay);
            tween->OnComplete(std::move(onComplete));
        }
        return handle;
    }

    TweenHandle TweenSystem::CreateSequence(std::vector<TweenHandle> tweenHandles)
    {
        if (tweenHandles.empty())
            return INVALID_TWEEN;

        for (size_t i = 1; i < tweenHandles.size(); ++i)
        {
            if (auto* t = Get(tweenHandles[i]))
                t->Pause();
        }

        for (size_t i = 0; i < tweenHandles.size() - 1; ++i)
        {
            TweenHandle nextHandle = tweenHandles[i + 1];
            if (auto* t = Get(tweenHandles[i]))
            {
                t->OnComplete([this, nextHandle]() { Resume(nextHandle); });
            }
        }

        return tweenHandles[0];
    }

    TweenHandle TweenSystem::CreateParallel(std::vector<TweenHandle> tweenHandles)
    {
        return tweenHandles.empty() ? INVALID_TWEEN : tweenHandles[0];
    }

    uint32_t TweenSystem::GetActiveTweenCount() const
    {
        return static_cast<uint32_t>(m_tweens.size());
    }

    std::string TweenSystem::Console_GetStatus() const
    {
        std::string status = "TweenSystem:\n";
        status += "  Active tweens: " + std::to_string(m_tweens.size()) + "\n";
        return status;
    }

    void TweenSystem::Shutdown()
    {
        m_tweens.clear();
        m_nextHandle = 1;
    }

} // namespace Spark
