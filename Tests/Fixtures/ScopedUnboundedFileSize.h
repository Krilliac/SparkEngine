/**
 * @file ScopedUnboundedFileSize.h
 * @brief Lifts the soft RLIMIT_FSIZE to the hard limit for a scope, then restores it.
 *
 * The sanitizer test wrapper (.github/scripts/run-sanitizer-tests.sh) caps the soft
 * RLIMIT_FSIZE at 16 MiB, inherited by every child process, and leaves the hard limit
 * alone so a test that must write a larger file can lift it. Two cases need that:
 *  - ModuleManager copies a module image into a private staging directory before
 *    dlopen, and sanitizer-instrumented Debug game modules exceed 16 MiB, so the copy
 *    fails with EFBIG (in process, or in a spawned engine child that inherits the cap);
 *  - fixtures that deliberately write oversized files would be silently truncated.
 *
 * Thread affinity: test thread; the limit is process-wide, so do not overlap scopes
 * across threads. No-op on Windows, which has no RLIMIT_FSIZE.
 */

#pragma once

#if !defined(_WIN32)
#include <sys/resource.h>
#endif

namespace SparkTestFixtures
{
    class ScopedUnboundedFileSize
    {
      public:
        ScopedUnboundedFileSize()
        {
#if !defined(_WIN32)
            m_saved = ::getrlimit(RLIMIT_FSIZE, &m_previous) == 0;
            if (m_saved && m_previous.rlim_cur != m_previous.rlim_max)
            {
                rlimit raised = m_previous;
                raised.rlim_cur = m_previous.rlim_max;
                ::setrlimit(RLIMIT_FSIZE, &raised);
            }
#endif
        }

        ~ScopedUnboundedFileSize()
        {
#if !defined(_WIN32)
            if (m_saved)
                ::setrlimit(RLIMIT_FSIZE, &m_previous);
#endif
        }

        ScopedUnboundedFileSize(const ScopedUnboundedFileSize&) = delete;
        ScopedUnboundedFileSize& operator=(const ScopedUnboundedFileSize&) = delete;

      private:
#if !defined(_WIN32)
        rlimit m_previous{};
        bool m_saved = false;
#endif
    };
} // namespace SparkTestFixtures
