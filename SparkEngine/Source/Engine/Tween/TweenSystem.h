/**
 * @file TweenSystem.h
 * @brief Advanced tween system with sequencing, composition, and easing
 * @author Spark Engine Team
 * @date 2026
 *
 * @details
 * Extends the engine's tween capabilities with handle-based management,
 * sequencing, parallel composition, and configurable looping. Complements
 * the simpler Utils/Tween.h system with production-ready lifecycle control.
 *
 * ## Usage
 * @code
 *   auto& ts = TweenSystem::GetInstance();
 *   ts.Initialize();
 *
 *   float opacity = 0.0f;
 *   auto handle = ts.TweenFloat(opacity, 0.0f, 1.0f, 0.5f, EaseType::EaseOutQuad);
 *
 *   // Each frame:
 *   ts.Update(deltaTime);
 * @endcode
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark
{

    // =========================================================================
    // Enumerations
    // =========================================================================

    /// @brief Easing function type for tween interpolation.
    enum class EaseType : uint8_t
    {
        Linear,
        EaseInQuad,
        EaseOutQuad,
        EaseInOutQuad,
        EaseInCubic,
        EaseOutCubic,
        EaseInOutCubic,
        EaseInElastic,
        EaseOutElastic,
        EaseOutBounce
    };

    /// @brief Current state of a tween.
    enum class TweenState : uint8_t
    {
        Pending,
        Running,
        Paused,
        Completed
    };

    /// @brief Loop behavior for a tween.
    enum class LoopMode : uint8_t
    {
        None,
        Loop,
        PingPong
    };

    /// @brief Apply the given ease function to a linear t value in [0, 1].
    float ApplyEase(EaseType ease, float t);

    // =========================================================================
    // TweenInstance
    // =========================================================================

    /**
     * @brief A single tween operation that interpolates a value over time.
     *
     * Named TweenInstance to avoid collision with Utils/Tween.h's Tween class.
     * Created via TweenSystem; configure using the chaining API after creation.
     */
    class TweenInstance
    {
      public:
        using UpdateCallback = std::function<void(float t)>;
        using CompleteCallback = std::function<void()>;

        TweenInstance() = default;
        TweenInstance(float duration, UpdateCallback onUpdate, EaseType ease = EaseType::Linear);

        TweenInstance& OnComplete(CompleteCallback callback);
        TweenInstance& SetLoop(LoopMode mode, int32_t count = -1);
        TweenInstance& SetDelay(float delay);
        TweenInstance& SetPlayRate(float rate);

        void Pause();
        void Resume();
        void Cancel();
        void Restart();

        TweenState GetState() const { return m_state; }
        float GetProgress() const { return (m_duration > 0.0f) ? (m_elapsed / m_duration) : 1.0f; }
        float GetElapsed() const { return m_elapsed; }

      private:
        friend class TweenSystem;

        bool Update(float dt);

        float m_duration = 0.0f;
        float m_elapsed = 0.0f;
        float m_delay = 0.0f;
        float m_playRate = 1.0f;
        EaseType m_ease = EaseType::Linear;
        LoopMode m_loopMode = LoopMode::None;
        int32_t m_loopCount = 0;
        int32_t m_currentLoop = 0;
        TweenState m_state = TweenState::Pending;
        UpdateCallback m_onUpdate;
        CompleteCallback m_onComplete;
    };

    // =========================================================================
    // TweenHandle
    // =========================================================================

    using TweenHandle = uint32_t;
    constexpr TweenHandle INVALID_TWEEN = 0;

    // =========================================================================
    // TweenSystem
    // =========================================================================

    /**
     * @brief Singleton system that manages handle-based tweens with composition.
     *
     * Provides sequencing, parallel playback, and lifecycle control beyond
     * what the simpler TweenManager offers.
     */
    class TweenSystem
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<TweenSystem>() instead")]]
        static TweenSystem& GetInstance();

        TweenSystem(const TweenSystem&) = delete;
        TweenSystem& operator=(const TweenSystem&) = delete;

        bool Initialize();
        void Shutdown();

        TweenHandle Create(float duration, TweenInstance::UpdateCallback onUpdate, EaseType ease = EaseType::Linear);
        TweenInstance* Get(TweenHandle handle);

        void Cancel(TweenHandle handle);
        void CancelAll();
        void Pause(TweenHandle handle);
        void Resume(TweenHandle handle);
        void Update(float deltaTime);

        uint32_t GetActiveTweenCount() const;

        TweenHandle TweenFloat(float& target, float from, float to, float duration, EaseType ease = EaseType::Linear);
        TweenHandle TweenDelay(float delay, TweenInstance::CompleteCallback onComplete);

        TweenHandle CreateSequence(std::vector<TweenHandle> tweens);
        TweenHandle CreateParallel(std::vector<TweenHandle> tweens);

        std::string Console_GetStatus() const;

      private:
        TweenSystem() = default;
        ~TweenSystem() = default;

        TweenHandle m_nextHandle = 1;
        std::unordered_map<TweenHandle, TweenInstance> m_tweens;
    };

} // namespace Spark
