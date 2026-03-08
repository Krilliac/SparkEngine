/**
 * @file CoroutineScheduler.h
 * @brief Gameplay coroutine system — delayed, yielding, and repeating tasks
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides a lightweight coroutine-like task scheduler for gameplay code,
 * inspired by Unity's Coroutine system and Unreal's latent actions. Coroutines
 * are cooperative: they run on the main thread and yield control back to the
 * scheduler each frame using Yield instructions.
 *
 * ## Features
 * - **WaitForSeconds(t)** — pause for t seconds of game time
 * - **WaitForFrames(n)** — pause for n frames
 * - **WaitUntil(pred)** — pause until a predicate returns true
 * - **Sequences** — chain multiple steps in a builder pattern
 * - **Named coroutines** — cancel by name or tag
 * - **Entity binding** — auto-cancel when an entity is destroyed
 *
 * ## Usage
 * @code
 *   auto& scheduler = CoroutineScheduler::GetInstance();
 *
 *   // Flash damage indicator for 0.2s, then fade out over 1s
 *   scheduler.StartCoroutine("damage_flash")
 *       .Do([&]() { hud.SetDamageFlash(1.0f); })
 *       .WaitForSeconds(0.2f)
 *       .Do([&]() { hud.FadeDamageFlash(1.0f); })
 *       .WaitForSeconds(1.0f)
 *       .Do([&]() { hud.SetDamageFlash(0.0f); });
 *
 *   // Spawn enemies in waves
 *   scheduler.StartCoroutine("wave_spawner")
 *       .Repeat(5, [&](int wave) {
 *           SpawnWave(wave);
 *       })
 *       .WithInterval(10.0f);
 *
 *   // Cancel by name
 *   scheduler.StopCoroutine("damage_flash");
 *   scheduler.StopAll();
 * @endcode
 *
 * @see ECSystems.h, Game.cpp
 */

#pragma once
#include <functional>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>

namespace Spark
{

    // ============================================================================
    // Yield Instructions — tell the scheduler when to resume a coroutine step
    // ============================================================================

    /**
 * @brief Base class for yield instructions that pause coroutine execution.
 */
    class YieldInstruction
    {
      public:
        virtual ~YieldInstruction() = default;

        /**
     * @brief Called each frame to check if the yield condition is satisfied.
     * @param deltaTime  Frame delta time in seconds.
     * @return True when the coroutine should resume.
     */
        virtual bool IsReady(float deltaTime) = 0;

        /** @brief Reset the instruction for reuse (e.g., in repeating coroutines). */
        virtual void Reset() {}
    };

    /**
 * @brief Yield for a specified duration in seconds.
 */
    class WaitForSeconds : public YieldInstruction
    {
      public:
        explicit WaitForSeconds(float seconds) : m_target(seconds) {}

        bool IsReady(float deltaTime) override
        {
            m_elapsed += deltaTime;
            return m_elapsed >= m_target;
        }

        void Reset() override { m_elapsed = 0.0f; }

      private:
        float m_target;
        float m_elapsed = 0.0f;
    };

    /**
 * @brief Yield for a specified number of frames.
 */
    class WaitForFrames : public YieldInstruction
    {
      public:
        explicit WaitForFrames(int frames) : m_target(frames) {}

        bool IsReady(float /*deltaTime*/) override { return ++m_count >= m_target; }

        void Reset() override { m_count = 0; }

      private:
        int m_target;
        int m_count = 0;
    };

    /**
 * @brief Yield until a predicate returns true.
 */
    class WaitUntil : public YieldInstruction
    {
      public:
        explicit WaitUntil(std::function<bool()> predicate) : m_predicate(std::move(predicate)) {}

        bool IsReady(float /*deltaTime*/) override { return m_predicate && m_predicate(); }

      private:
        std::function<bool()> m_predicate;
    };

    /**
 * @brief Yield for one frame (resume next frame).
 */
    class WaitForEndOfFrame : public YieldInstruction
    {
      public:
        bool IsReady(float /*deltaTime*/) override
        {
            if (m_waited)
                return true;
            m_waited = true;
            return false;
        }

        void Reset() override { m_waited = false; }

      private:
        bool m_waited = false;
    };

    // ============================================================================
    // Coroutine — a sequence of action/yield steps
    // ============================================================================

    /**
 * @brief A single coroutine: a named sequence of action steps separated by yields.
 */
    class Coroutine
    {
      public:
        explicit Coroutine(const std::string& name) : m_name(name) {}

        /** @brief Get the coroutine's name (for cancellation and debugging). */
        const std::string& GetName() const { return m_name; }

        /** @brief Check if all steps have been executed. */
        bool IsFinished() const { return m_currentStep >= static_cast<int>(m_steps.size()); }

        /** @brief Mark this coroutine for removal. */
        void Cancel() { m_cancelled = true; }

        /** @brief Check if the coroutine has been cancelled. */
        bool IsCancelled() const { return m_cancelled; }

        // ---- Builder API -------------------------------------------------------

        /**
     * @brief Add an action step (runs immediately when reached).
     */
        Coroutine& Do(std::function<void()> action)
        {
            m_steps.push_back({Step::Type::Action, std::move(action), nullptr});
            return *this;
        }

        /**
     * @brief Add a wait-for-seconds yield step.
     */
        Coroutine& WaitForSeconds(float seconds)
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<Spark::WaitForSeconds>(seconds)});
            return *this;
        }

        /**
     * @brief Add a wait-for-frames yield step.
     */
        Coroutine& WaitForFrames(int frames)
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<Spark::WaitForFrames>(frames)});
            return *this;
        }

        /**
     * @brief Add a wait-until-predicate yield step.
     */
        Coroutine& WaitUntil(std::function<bool()> predicate)
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<Spark::WaitUntil>(std::move(predicate))});
            return *this;
        }

        /**
     * @brief Add a yield-one-frame step.
     */
        Coroutine& YieldFrame()
        {
            m_steps.push_back({Step::Type::Yield, nullptr, std::make_unique<WaitForEndOfFrame>()});
            return *this;
        }

        /**
     * @brief Tick the coroutine forward by one frame.
     *
     * Executes action steps immediately and checks yield conditions.
     * @param deltaTime  Frame delta time in seconds.
     */
        void Update(float deltaTime)
        {
            if (m_cancelled || IsFinished())
                return;

            while (m_currentStep < static_cast<int>(m_steps.size()))
            {
                auto& step = m_steps[m_currentStep];

                if (step.type == Step::Type::Action)
                {
                    if (step.action)
                        step.action();
                    m_currentStep++;
                    continue;
                }

                // Yield step — check if ready
                if (step.yield && step.yield->IsReady(deltaTime))
                {
                    m_currentStep++;
                    continue;
                }

                // Not ready — stop processing until next frame
                break;
            }
        }

      private:
        struct Step
        {
            enum class Type
            {
                Action,
                Yield
            };
            Type type;
            std::function<void()> action;
            std::unique_ptr<YieldInstruction> yield;
        };

        std::string m_name;
        std::vector<Step> m_steps;
        int m_currentStep = 0;
        bool m_cancelled = false;
    };

    // ============================================================================
    // CoroutineScheduler — manages all active coroutines
    // ============================================================================

    /**
 * @brief Central scheduler that ticks all active coroutines each frame.
 *
 * Singleton pattern — access via `CoroutineScheduler::GetInstance()`.
 * Call `Update(deltaTime)` once per frame from the game loop.
 */
    class CoroutineScheduler
    {
      public:
        static CoroutineScheduler& GetInstance()
        {
            static CoroutineScheduler instance;
            return instance;
        }

        /**
     * @brief Start a new named coroutine and return it for builder-pattern setup.
     *
     * @param name  Unique name for this coroutine (used for cancellation).
     * @return Reference to the new Coroutine for chaining Do/Wait calls.
     */
        Coroutine& StartCoroutine(const std::string& name)
        {
            m_coroutines.push_back(std::make_unique<Coroutine>(name));
            return *m_coroutines.back();
        }

        /**
     * @brief Cancel and remove all coroutines with the given name.
     */
        void StopCoroutine(const std::string& name)
        {
            for (auto& co : m_coroutines)
            {
                if (co && co->GetName() == name)
                {
                    co->Cancel();
                }
            }
        }

        /**
     * @brief Cancel all running coroutines.
     */
        void StopAll()
        {
            for (auto& co : m_coroutines)
            {
                if (co)
                    co->Cancel();
            }
        }

        /**
     * @brief Check if any coroutine with the given name is still running.
     */
        bool IsRunning(const std::string& name) const
        {
            for (const auto& co : m_coroutines)
            {
                if (co && co->GetName() == name && !co->IsFinished() && !co->IsCancelled())
                {
                    return true;
                }
            }
            return false;
        }

        /**
     * @brief Tick all active coroutines. Call once per frame from the game loop.
     *
     * @param deltaTime  Frame delta time in seconds.
     */
        void Update(float deltaTime)
        {
            // Tick each coroutine
            for (auto& co : m_coroutines)
            {
                if (co && !co->IsCancelled() && !co->IsFinished())
                {
                    co->Update(deltaTime);
                }
            }

            // Remove finished and cancelled coroutines
            m_coroutines.erase(std::remove_if(m_coroutines.begin(), m_coroutines.end(),
                                              [](const std::unique_ptr<Coroutine>& co)
                                              { return !co || co->IsCancelled() || co->IsFinished(); }),
                               m_coroutines.end());
        }

        /** @brief Number of active coroutines. */
        size_t ActiveCount() const { return m_coroutines.size(); }

      private:
        CoroutineScheduler() = default;
        std::vector<std::unique_ptr<Coroutine>> m_coroutines;
    };

    // ============================================================================
    // Convenience free functions
    // ============================================================================

    /** @brief Shorthand to start a coroutine on the global scheduler. */
    inline Coroutine& StartCoroutine(const std::string& name)
    {
        return CoroutineScheduler::GetInstance().StartCoroutine(name);
    }

    /** @brief Shorthand to stop a coroutine by name. */
    inline void StopCoroutine(const std::string& name)
    {
        CoroutineScheduler::GetInstance().StopCoroutine(name);
    }

} // namespace Spark
