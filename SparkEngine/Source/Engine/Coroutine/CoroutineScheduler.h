/**
 * @file CoroutineScheduler.h
 * @brief Gameplay coroutine system -- delayed, yielding, and repeating tasks
 * @author Spark Engine Team
 * @date 2025
 *
 * Provides both a builder-pattern coroutine scheduler and C++20 coroutine
 * integration for gameplay code. Coroutines are cooperative: they run on the
 * main thread and yield control back to the scheduler each frame.
 *
 * ## Builder-pattern API
 * @code
 *   auto& scheduler = CoroutineScheduler::GetInstance();
 *
 *   scheduler.StartCoroutine("damage_flash")
 *       .Do([&]() { hud.SetDamageFlash(1.0f); })
 *       .WaitForSeconds(0.2f)
 *       .Do([&]() { hud.FadeDamageFlash(1.0f); })
 *       .WaitForSeconds(1.0f)
 *       .Do([&]() { hud.SetDamageFlash(0.0f); });
 * @endcode
 *
 * ## C++20 coroutine API
 * @code
 *   GameCoroutine SpawnWaves(int count)
 *   {
 *       for (int i = 0; i < count; ++i)
 *       {
 *           SpawnWave(i);
 *           co_await Spark::WaitForSeconds(10.0f);
 *       }
 *       co_return;
 *   }
 *
 *   // Schedule on the global scheduler:
 *   CoroutineScheduler::GetInstance().Schedule("waves", SpawnWaves(5));
 * @endcode
 *
 * @see CoroutineTypes.h, EventSystem.h, ECSystems.h, Game.cpp
 */

#pragma once
#include "CoroutineTypes.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace Spark
{

    // ============================================================================
    // CoroutineScheduler -- manages all active coroutines
    // ============================================================================

    /**
     * @brief Central scheduler that ticks all active coroutines each frame.
     *
     * Singleton pattern -- access via `CoroutineScheduler::GetInstance()`.
     * Call `Update(deltaTime)` once per frame from the game loop.
     *
     * Supports both builder-pattern coroutines and C++20 native coroutines.
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
         * @brief Schedule a C++20 GameCoroutine on the scheduler.
         *
         * @param name       Name for cancellation and debugging.
         * @param coroutine  A GameCoroutine returned from a coroutine function.
         */
        void Schedule(const std::string& name, GameCoroutine coroutine)
        {
            m_nativeCoroutines.push_back(std::make_unique<NativeCoroutineWrapper>(name, std::move(coroutine)));
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
            for (auto& co : m_nativeCoroutines)
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
            for (auto& co : m_nativeCoroutines)
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
            for (const auto& co : m_nativeCoroutines)
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
            // Tick builder-pattern coroutines
            for (auto& co : m_coroutines)
            {
                if (co && !co->IsCancelled() && !co->IsFinished())
                {
                    co->Update(deltaTime);
                }
            }

            // Tick C++20 native coroutines
            for (auto& co : m_nativeCoroutines)
            {
                if (co && !co->IsCancelled() && !co->IsFinished())
                {
                    co->Update(deltaTime);
                }
            }

            // Remove finished and cancelled builder coroutines
            m_coroutines.erase(std::remove_if(m_coroutines.begin(), m_coroutines.end(),
                                              [](const std::unique_ptr<Coroutine>& co)
                                              { return !co || co->IsCancelled() || co->IsFinished(); }),
                               m_coroutines.end());

            // Remove finished and cancelled native coroutines
            m_nativeCoroutines.erase(std::remove_if(m_nativeCoroutines.begin(), m_nativeCoroutines.end(),
                                                    [](const std::unique_ptr<NativeCoroutineWrapper>& co)
                                                    { return !co || co->IsCancelled() || co->IsFinished(); }),
                                     m_nativeCoroutines.end());
        }

        /** @brief Number of active coroutines (builder + native). */
        size_t ActiveCount() const { return m_coroutines.size() + m_nativeCoroutines.size(); }

      private:
        CoroutineScheduler() = default;
        std::vector<std::unique_ptr<Coroutine>> m_coroutines;
        std::vector<std::unique_ptr<NativeCoroutineWrapper>> m_nativeCoroutines;
    };

    // ============================================================================
    // Convenience free functions
    // ============================================================================

    /** @brief Shorthand to start a builder-pattern coroutine on the global scheduler. */
    inline Coroutine& StartCoroutine(const std::string& name)
    {
        return CoroutineScheduler::GetInstance().StartCoroutine(name);
    }

    /** @brief Shorthand to stop a coroutine by name. */
    inline void StopCoroutine(const std::string& name)
    {
        CoroutineScheduler::GetInstance().StopCoroutine(name);
    }

    /** @brief Shorthand to schedule a C++20 coroutine on the global scheduler. */
    inline void ScheduleCoroutine(const std::string& name, GameCoroutine coroutine)
    {
        CoroutineScheduler::GetInstance().Schedule(name, std::move(coroutine));
    }

} // namespace Spark
