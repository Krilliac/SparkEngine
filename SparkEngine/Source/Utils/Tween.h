/**
 * @file Tween.h
 * @brief Tweening and easing library for smooth property interpolation
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a lightweight tweening system (inspired by DOTween/LeanTween from Unity)
 * for smoothly interpolating numeric values, positions, rotations, colors, and
 * other properties over time using a variety of easing functions.
 *
 * ## Easing Functions
 * All 30 standard Robert Penner easing equations are included:
 * Linear, Quad, Cubic, Quart, Quint, Sine, Expo, Circ, Back, Elastic, Bounce
 * — each with In, Out, and InOut variants.
 *
 * ## Usage
 * @code
 *   auto& tweener = TweenManager::GetInstance();
 *
 *   // Tween a float from 0 to 100 over 2 seconds with ease-out-cubic
 *   float opacity = 0.0f;
 *   tweener.To(&opacity, 100.0f, 2.0f, Ease::OutCubic);
 *
 *   // Tween with callbacks
 *   tweener.To(&position.x, targetX, 1.0f, Ease::InOutQuad)
 *       .OnComplete([]() { PlaySound("arrive"); })
 *       .SetDelay(0.5f)
 *       .SetLoops(3, LoopType::PingPong);
 * @endcode
 *
 * @see CoroutineScheduler.h, AnimationSystem.h
 */

#pragma once
#include <cmath>
#include <vector>
#include <memory>
#include <functional>
#include <algorithm>
#include <string>

namespace Spark
{

    // ============================================================================
    // Easing Functions
    // ============================================================================

    /** @brief Available easing curve types. */
    enum class Ease
    {
        Linear,
        InQuad,
        OutQuad,
        InOutQuad,
        InCubic,
        OutCubic,
        InOutCubic,
        InQuart,
        OutQuart,
        InOutQuart,
        InQuint,
        OutQuint,
        InOutQuint,
        InSine,
        OutSine,
        InOutSine,
        InExpo,
        OutExpo,
        InOutExpo,
        InCirc,
        OutCirc,
        InOutCirc,
        InBack,
        OutBack,
        InOutBack,
        InElastic,
        OutElastic,
        InOutElastic,
        InBounce,
        OutBounce,
        InOutBounce
    };

    /**
 * @brief Evaluate an easing function at normalized time t ∈ [0, 1].
 * @param ease  The easing curve to use.
 * @param t     Normalized time (0 = start, 1 = end).
 * @return Eased value, typically in [0, 1] but may overshoot for Back/Elastic.
 */
    inline float EaseEvaluate(Ease ease, float t)
    {
        constexpr float PI = 3.14159265358979323846f;

        switch (ease)
        {
        case Ease::Linear:
            return t;

        // Quad
        case Ease::InQuad:
            return t * t;
        case Ease::OutQuad:
            return t * (2.0f - t);
        case Ease::InOutQuad:
            return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;

        // Cubic
        case Ease::InCubic:
            return t * t * t;
        case Ease::OutCubic:
        {
            float u = t - 1.0f;
            return u * u * u + 1.0f;
        }
        case Ease::InOutCubic:
            return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;

        // Quart
        case Ease::InQuart:
            return t * t * t * t;
        case Ease::OutQuart:
        {
            float u = t - 1.0f;
            return 1.0f - u * u * u * u;
        }
        case Ease::InOutQuart:
            return t < 0.5f ? 8.0f * t * t * t * t : 1.0f - 8.0f * (t - 1.0f) * (t - 1.0f) * (t - 1.0f) * (t - 1.0f);

        // Quint
        case Ease::InQuint:
            return t * t * t * t * t;
        case Ease::OutQuint:
        {
            float u = t - 1.0f;
            return 1.0f + u * u * u * u * u;
        }
        case Ease::InOutQuint:
            return t < 0.5f ? 16.0f * t * t * t * t * t
                            : 1.0f + 16.0f * (t - 1.0f) * (t - 1.0f) * (t - 1.0f) * (t - 1.0f) * (t - 1.0f);

        // Sine
        case Ease::InSine:
            return 1.0f - std::cos(t * PI * 0.5f);
        case Ease::OutSine:
            return std::sin(t * PI * 0.5f);
        case Ease::InOutSine:
            return 0.5f * (1.0f - std::cos(PI * t));

        // Expo
        case Ease::InExpo:
            return t == 0.0f ? 0.0f : std::pow(2.0f, 10.0f * (t - 1.0f));
        case Ease::OutExpo:
            return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t);
        case Ease::InOutExpo:
            if (t == 0.0f)
                return 0.0f;
            if (t == 1.0f)
                return 1.0f;
            return t < 0.5f ? 0.5f * std::pow(2.0f, 20.0f * t - 10.0f)
                            : 1.0f - 0.5f * std::pow(2.0f, -20.0f * t + 10.0f);

        // Circ
        case Ease::InCirc:
            return 1.0f - std::sqrt(1.0f - t * t);
        case Ease::OutCirc:
            return std::sqrt(1.0f - (t - 1.0f) * (t - 1.0f));
        case Ease::InOutCirc:
            return t < 0.5f ? 0.5f * (1.0f - std::sqrt(1.0f - 4.0f * t * t))
                            : 0.5f * (std::sqrt(1.0f - (2.0f * t - 2.0f) * (2.0f * t - 2.0f)) + 1.0f);

        // Back (overshoot)
        case Ease::InBack:
        {
            constexpr float s = 1.70158f;
            return t * t * ((s + 1.0f) * t - s);
        }
        case Ease::OutBack:
        {
            constexpr float s = 1.70158f;
            float u = t - 1.0f;
            return u * u * ((s + 1.0f) * u + s) + 1.0f;
        }
        case Ease::InOutBack:
        {
            constexpr float s = 1.70158f * 1.525f;
            float u = t * 2.0f;
            if (u < 1.0f)
                return 0.5f * (u * u * ((s + 1.0f) * u - s));
            u -= 2.0f;
            return 0.5f * (u * u * ((s + 1.0f) * u + s) + 2.0f);
        }

        // Elastic
        case Ease::InElastic:
            if (t == 0.0f || t == 1.0f)
                return t;
            return -std::pow(2.0f, 10.0f * t - 10.0f) * std::sin((t * 10.0f - 10.75f) * (2.0f * PI / 3.0f));
        case Ease::OutElastic:
            if (t == 0.0f || t == 1.0f)
                return t;
            return std::pow(2.0f, -10.0f * t) * std::sin((t * 10.0f - 0.75f) * (2.0f * PI / 3.0f)) + 1.0f;
        case Ease::InOutElastic:
            if (t == 0.0f || t == 1.0f)
                return t;
            return t < 0.5f ? -0.5f * std::pow(2.0f, 20.0f * t - 10.0f) *
                                  std::sin((20.0f * t - 11.125f) * (2.0f * PI / 4.5f))
                            : 0.5f * std::pow(2.0f, -20.0f * t + 10.0f) *
                                      std::sin((20.0f * t - 11.125f) * (2.0f * PI / 4.5f)) +
                                  1.0f;

        // Bounce
        case Ease::OutBounce:
        {
            if (t < 1.0f / 2.75f)
                return 7.5625f * t * t;
            if (t < 2.0f / 2.75f)
            {
                float u = t - 1.5f / 2.75f;
                return 7.5625f * u * u + 0.75f;
            }
            if (t < 2.5f / 2.75f)
            {
                float u = t - 2.25f / 2.75f;
                return 7.5625f * u * u + 0.9375f;
            }
            {
                float u = t - 2.625f / 2.75f;
                return 7.5625f * u * u + 0.984375f;
            }
        }
        case Ease::InBounce:
            return 1.0f - EaseEvaluate(Ease::OutBounce, 1.0f - t);
        case Ease::InOutBounce:
            return t < 0.5f ? 0.5f * EaseEvaluate(Ease::InBounce, t * 2.0f)
                            : 0.5f * EaseEvaluate(Ease::OutBounce, t * 2.0f - 1.0f) + 0.5f;
        }

        return t; // fallback
    }

    // ============================================================================
    // Loop types
    // ============================================================================

    /** @brief How a tween behaves when it reaches the end. */
    enum class LoopType
    {
        Restart,  ///< Jump back to start and play again
        PingPong, ///< Reverse direction each cycle
        Yoyo      ///< Alias for PingPong
    };

    // ============================================================================
    // Tween — a single animated property
    // ============================================================================

    /**
 * @brief A single tween that interpolates a float* from its current value to a target.
 */
    class Tween
    {
      public:
        Tween(float* target, float endValue, float duration, Ease ease = Ease::Linear)
            : m_target(target), m_startValue(target ? *target : 0.0f), m_endValue(endValue), m_duration(duration),
              m_ease(ease)
        {
        }

        // ---- Builder API -------------------------------------------------------

        /** @brief Set a delay before the tween starts. */
        Tween& SetDelay(float delay)
        {
            m_delay = delay;
            return *this;
        }

        /** @brief Set the number of loops. -1 = infinite. */
        Tween& SetLoops(int count, LoopType type = LoopType::Restart)
        {
            m_loopCount = count;
            m_loopType = type;
            return *this;
        }

        /** @brief Callback invoked each frame the tween updates. */
        Tween& OnUpdate(std::function<void(float)> callback)
        {
            m_onUpdate = std::move(callback);
            return *this;
        }

        /** @brief Callback invoked when the tween completes (all loops). */
        Tween& OnComplete(std::function<void()> callback)
        {
            m_onComplete = std::move(callback);
            return *this;
        }

        /** @brief Immediately kill this tween. */
        void Kill() { m_killed = true; }

        /** @brief Is this tween finished (or killed)? */
        bool IsFinished() const { return m_killed || m_finished; }

        /**
     * @brief Advance the tween by deltaTime.
     * @return True if the tween is still active; false if finished.
     */
        bool Update(float deltaTime)
        {
            if (m_killed || m_finished)
                return false;

            // Handle delay
            if (m_delayRemaining > 0.0f)
            {
                m_delayRemaining -= deltaTime;
                if (m_delayRemaining > 0.0f)
                    return true;
                deltaTime = -m_delayRemaining; // carry over
            }

            m_elapsed += deltaTime;

            // Compute normalized time within current loop
            float t = (m_duration > 0.0f) ? std::min(m_elapsed / m_duration, 1.0f) : 1.0f;

            // Apply ping-pong
            if (m_pingPongReverse)
                t = 1.0f - t;

            // Apply easing
            float eased = EaseEvaluate(m_ease, t);
            float value = m_startValue + (m_endValue - m_startValue) * eased;

            // Write to target
            if (m_target)
                *m_target = value;
            if (m_onUpdate)
                m_onUpdate(value);

            // Check for loop completion
            if (m_elapsed >= m_duration)
            {
                m_completedLoops++;

                if (m_loopCount == -1 || m_completedLoops < m_loopCount)
                {
                    // Start next loop
                    m_elapsed = 0.0f;

                    if (m_loopType == LoopType::PingPong || m_loopType == LoopType::Yoyo)
                    {
                        m_pingPongReverse = !m_pingPongReverse;
                    }
                    else
                    {
                        // Restart: reset to start value
                        if (m_target)
                            *m_target = m_startValue;
                    }
                }
                else
                {
                    // Finished
                    if (m_target)
                        *m_target = m_pingPongReverse ? m_startValue : m_endValue;
                    m_finished = true;
                    if (m_onComplete)
                        m_onComplete();
                    return false;
                }
            }

            return true;
        }

      private:
        float* m_target;
        float m_startValue;
        float m_endValue;
        float m_duration;
        Ease m_ease;

        float m_delay = 0.0f;
        float m_delayRemaining = 0.0f;
        float m_elapsed = 0.0f;

        int m_loopCount = 1; // 1 = play once, -1 = infinite
        int m_completedLoops = 0;
        LoopType m_loopType = LoopType::Restart;
        bool m_pingPongReverse = false;

        bool m_killed = false;
        bool m_finished = false;

        std::function<void(float)> m_onUpdate;
        std::function<void()> m_onComplete;
    };

    // ============================================================================
    // TweenManager — global tween scheduler
    // ============================================================================

    /**
 * @brief Central manager for all active tweens. Call Update() once per frame.
 */
    class TweenManager
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<TweenManager>() instead")]]
        static TweenManager& GetInstance()
        {
            static TweenManager instance;
            return instance;
        }

        /**
     * @brief Create a tween that interpolates *target to endValue over duration.
     * @return Reference to the tween for builder-pattern configuration.
     */
        Tween& To(float* target, float endValue, float duration, Ease ease = Ease::OutQuad)
        {
            m_tweens.push_back(std::make_unique<Tween>(target, endValue, duration, ease));
            return *m_tweens.back();
        }

        /**
     * @brief Tick all active tweens. Call once per frame.
     */
        void Update(float deltaTime)
        {
            m_tweens.erase(std::remove_if(m_tweens.begin(), m_tweens.end(), [deltaTime](std::unique_ptr<Tween>& tween)
                                          { return !tween->Update(deltaTime); }),
                           m_tweens.end());
        }

        /** @brief Kill all active tweens. */
        void KillAll()
        {
            for (auto& t : m_tweens)
                t->Kill();
            m_tweens.clear();
        }

        /** @brief Number of active tweens. */
        size_t ActiveCount() const { return m_tweens.size(); }

      private:
        TweenManager() = default;
        std::vector<std::unique_ptr<Tween>> m_tweens;
    };

} // namespace Spark
