/**
 * @file LifecycleLoopGuards.h
 * @brief Shared guards for the LIFE-200 repeated-lifecycle loop tests.
 *
 * CountProcessThreads() reads the live OS thread count, so a loop can prove a
 * boot/shutdown or load/unload cycle does not leave a thread behind.
 * CycleWatchdog turns a deadlocked cycle into an immediate, named process
 * failure instead of a silent CTest timeout.
 *
 * Include this header after every engine header: on Windows it pulls in
 * <windows.h> with WIN32_LEAN_AND_MEAN and NOMINMAX.
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#else
#include <filesystem>
#endif

namespace LifecycleLoop
{
    /**
     * Threads the OS may start or retire on its own between two samples (the
     * Windows loader and thread-pool workers). A per-cycle leak over the loops'
     * 20+ cycles grows far past this, so the slack cannot hide one.
     */
    inline constexpr std::size_t kOsThreadSlack = 4;

    /// Live threads in this process; 0 only when the OS query itself failed.
    inline std::size_t CountProcessThreads()
    {
#if defined(_WIN32)
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
            return 0;
        const DWORD processId = GetCurrentProcessId();
        std::size_t count = 0;
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID == processId)
                ++count;
        }
        CloseHandle(snapshot);
        return count;
#elif defined(__APPLE__)
        thread_act_array_t threads = nullptr;
        mach_msg_type_number_t count = 0;
        if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS)
            return 0;
        for (mach_msg_type_number_t i = 0; i < count; ++i)
            mach_port_deallocate(mach_task_self(), threads[i]);
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads), sizeof(thread_act_t) * count);
        return static_cast<std::size_t>(count);
#else
        std::error_code ec;
        std::size_t count = 0;
        for (std::filesystem::directory_iterator it("/proc/self/task", ec), end; !ec && it != end; it.increment(ec))
            ++count;
        return ec ? 0 : count;
#endif
    }

    /**
     * @brief Fails the process when one armed cycle outlives its deadline.
     *
     * The loop runs on the test thread (the lifecycle stages declare main-thread
     * affinity); this watchdog thread only waits. Construct it before taking a
     * thread-count baseline so its own thread is part of the baseline.
     */
    class CycleWatchdog final
    {
      public:
        explicit CycleWatchdog(std::chrono::seconds deadline) : m_deadline(deadline), m_thread([this] { Run(); }) {}

        ~CycleWatchdog()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_wake.notify_all();
            m_thread.join();
        }

        CycleWatchdog(const CycleWatchdog&) = delete;
        CycleWatchdog& operator=(const CycleWatchdog&) = delete;

        /// Start timing @p label; a cycle still armed at the deadline aborts the run.
        void Arm(std::string label)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_label = std::move(label);
                m_expiry = std::chrono::steady_clock::now() + m_deadline;
                m_armed = true;
            }
            m_wake.notify_all();
        }

        void Disarm()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_armed = false;
        }

      private:
        void Run()
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            while (!m_stop)
            {
                if (!m_armed)
                {
                    m_wake.wait(lock);
                    continue;
                }
                const auto expiry = m_expiry;
                if (m_wake.wait_until(lock, expiry) == std::cv_status::timeout && m_armed && !m_stop &&
                    m_expiry == expiry)
                {
                    std::fprintf(stderr, "[LIFE-200] deadlock: %s did not finish within %lld s\n", m_label.c_str(),
                                 static_cast<long long>(m_deadline.count()));
                    std::fflush(stderr);
                    std::_Exit(3);
                }
            }
        }

        const std::chrono::seconds m_deadline;
        std::mutex m_mutex;
        std::condition_variable m_wake;
        std::string m_label;
        std::chrono::steady_clock::time_point m_expiry{};
        bool m_armed = false;
        bool m_stop = false;
        std::thread m_thread; // Last: starts after every member above is constructed.
    };
} // namespace LifecycleLoop
