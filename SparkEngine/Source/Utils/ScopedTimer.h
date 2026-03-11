/**
 * @file ScopedTimer.h
 * @brief RAII-based scoped timing utility for quick performance measurements
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a lightweight RAII timer that measures the time a scope takes
 * to execute and reports it via a user-provided callback. Complements the
 * full Profiler for one-off measurements during development.
 *
 * ## Usage
 * @code
 *   {
 *       Spark::ScopedTimer timer("LoadLevel", [](const char* name, float ms) {
 *           printf("%s took %.2f ms\n", name, ms);
 *       });
 *       LoadLevel("map01");
 *   } // prints "LoadLevel took 142.35 ms"
 *
 *   // With the convenience macro
 *   void ProcessAI() {
 *       SPARK_SCOPED_TIMER("ProcessAI");
 *       // ... AI logic ...
 *   }
 * @endcode
 */

#pragma once

#include <chrono>
#include <cstdio>
#include <functional>

namespace Spark
{

    /**
     * @class ScopedTimer
     * @brief Measures elapsed time from construction to destruction.
     *
     * On destruction, invokes the callback with the scope name and elapsed
     * time in milliseconds. If no callback is provided, prints to stdout.
     */
    class ScopedTimer
    {
      public:
        using Callback = std::function<void(const char* name, float milliseconds)>;

        /**
         * @brief Construct and start timing.
         * @param name      Descriptive label for the scope being timed.
         * @param callback  Function called on destruction with results.
         *                  If null, a default printf callback is used.
         */
        explicit ScopedTimer(const char* name, Callback callback = nullptr)
            : m_name(name), m_callback(std::move(callback)), m_start(std::chrono::high_resolution_clock::now())
        {
        }

        ~ScopedTimer()
        {
            auto end = std::chrono::high_resolution_clock::now();
            float ms = std::chrono::duration<float, std::milli>(end - m_start).count();

            if (m_callback)
            {
                m_callback(m_name, ms);
            }
            else
            {
                std::printf("[ScopedTimer] %s: %.3f ms\n", m_name, ms);
            }
        }

        // Non-copyable, non-movable
        ScopedTimer(const ScopedTimer&) = delete;
        ScopedTimer& operator=(const ScopedTimer&) = delete;
        ScopedTimer(ScopedTimer&&) = delete;
        ScopedTimer& operator=(ScopedTimer&&) = delete;

        /**
         * @brief Get elapsed time so far without stopping.
         * @return  Elapsed milliseconds since construction.
         */
        float ElapsedMs() const
        {
            auto now = std::chrono::high_resolution_clock::now();
            return std::chrono::duration<float, std::milli>(now - m_start).count();
        }

      private:
        const char* m_name;
        Callback m_callback;
        std::chrono::high_resolution_clock::time_point m_start;
    };

} // namespace Spark

/**
 * @brief Convenience macro: creates a ScopedTimer that prints to stdout.
 * @param name  String literal label for the timed scope.
 */
#define SPARK_SCOPED_TIMER(name) ::Spark::ScopedTimer _scopedTimer_##__LINE__(name)
