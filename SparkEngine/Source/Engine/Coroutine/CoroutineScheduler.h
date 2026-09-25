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
         *
         * Outside Update() the cancelled coroutines are destroyed before this returns, so an owner
         * whose step callables live in a game-module image can stop them and then unload that image:
         * nothing left in the scheduler still points into it. From inside Update() (a step stopping
         * itself or a sibling) destruction is deferred to the end of that tick.
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
            if (!m_updating)
                RemoveCancelled();
        }

        /**
         * @brief Cancel all running coroutines (destroyed immediately outside Update(), as StopCoroutine).
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
            if (!m_updating)
                RemoveCancelled();
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
            // Iterate by index over a cached size rather than range-for: a ticking
            // coroutine may call StartCoroutine()/Schedule(), which push_back onto
            // these same vectors and can reallocate them. Caching the count means
            // coroutines spawned mid-tick are deferred to the next frame (and the
            // reallocation cannot invalidate a live range-for iterator). The pointed-to
            // Coroutine objects live on the heap behind unique_ptr, so a reallocation
            // moves the unique_ptr slots but never the object a tick is running inside.

            // A step may call StopCoroutine()/StopAll(); removal must then wait until
            // the loops below are done with the vectors.
            struct UpdatingScope
            {
                bool& updating;
                bool previous;
                explicit UpdatingScope(bool& flag) : updating(flag), previous(flag) { updating = true; }
                ~UpdatingScope() { updating = previous; }
                UpdatingScope(const UpdatingScope&) = delete;
                UpdatingScope& operator=(const UpdatingScope&) = delete;
            } updatingScope{m_updating};

            // Tick builder-pattern coroutines
            for (size_t i = 0, n = m_coroutines.size(); i < n; ++i)
            {
                auto& co = m_coroutines[i];
                if (co && !co->IsCancelled() && !co->IsFinished())
                {
                    co->Update(deltaTime);
                }
            }

            // Tick C++20 native coroutines
            for (size_t i = 0, n = m_nativeCoroutines.size(); i < n; ++i)
            {
                auto& co = m_nativeCoroutines[i];
                if (co && !co->IsCancelled() && !co->IsFinished())
                {
                    co->Update(deltaTime);
                }
            }

            // Nested Update() from inside a step leaves removal to the outermost tick.
            if (!updatingScope.previous)
                RemoveInactive();
        }

        /** @brief Number of active coroutines (builder + native). */
        size_t ActiveCount() const { return m_coroutines.size() + m_nativeCoroutines.size(); }

      private:
        CoroutineScheduler() = default;

        /** @brief Destroy every finished or cancelled coroutine. Never called while Update() iterates. */
        void RemoveInactive()
        {
            std::erase_if(m_coroutines, [](const std::unique_ptr<Coroutine>& co)
                          { return !co || co->IsCancelled() || co->IsFinished(); });
            std::erase_if(m_nativeCoroutines, [](const std::unique_ptr<NativeCoroutineWrapper>& co)
                          { return !co || co->IsCancelled() || co->IsFinished(); });
        }

        /**
         * @brief Destroy only cancelled coroutines (the Stop*() path outside Update()).
         *
         * Finished entries are left for the next Update(): a builder coroutine whose steps are still being
         * chained reports IsFinished() (zero steps), so erasing finished entries here would invalidate the
         * reference StartCoroutine() returned to a caller that has not added its first step yet.
         */
        void RemoveCancelled()
        {
            std::erase_if(m_coroutines, [](const std::unique_ptr<Coroutine>& co) { return !co || co->IsCancelled(); });
            std::erase_if(m_nativeCoroutines,
                          [](const std::unique_ptr<NativeCoroutineWrapper>& co) { return !co || co->IsCancelled(); });
        }

        std::vector<std::unique_ptr<Coroutine>> m_coroutines;
        std::vector<std::unique_ptr<NativeCoroutineWrapper>> m_nativeCoroutines;
        bool m_updating = false;
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
