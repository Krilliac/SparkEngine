/**
 * @file MiniDumpWithoutStacks.h
 * @brief Minidump writer that omits every thread's stack memory (OPS-100).
 *
 * Thread stacks hold live locals (passwords, SCRAM keys, session tokens). MiniDumpNormal copies
 * them, MiniDumpFilterMemory still copies them verbatim, and DbgHelp ignores a cleared
 * ThreadWriteStack flag, so the only reliable way to keep them out is to hand every thread's
 * stack back through RemoveMemoryCallback. The whole reservation goes (from its allocation base to
 * StackBase), so neither the reported stack pointer nor stale frames below it can put stack bytes
 * in the file. Registers, the instruction window, the module and thread lists and the exception
 * record stay in the dump.
 *
 * DbgHelp never runs on the requesting thread. Microsoft documents in-process MiniDumpWriteDump as
 * unreliable on the calling thread. A dedicated writer thread whose fresh stack never held a caller
 * secret runs it instead; the IncludeThreadCallback keeps that thread out of the dump, and a
 * memory read that fails because the page vanished while the process kept running is omitted
 * rather than failing the dump.
 *
 * DbgHelp never reads the caller's exception structures either. Some DbgHelp builds read the
 * exception ContextRecord at the full size of every extended state the processor enables (several
 * KiB past a plain CONTEXT, or past a kernel context sized for the state the thread had in use)
 * and write those bytes into the dump's exception context, where RemoveMemoryCallback cannot reach
 * them. A caller's CONTEXT sits in a stack frame next to live locals, so the record, the pointers
 * and the context are copied into writer-owned static storage first; the context goes at the start
 * of a 64 KiB zero-filled buffer, larger than any XSAVE layout, so every byte DbgHelp reads there
 * is the writer's.
 *
 * Contract: Windows only. Crash-path safe: the job and the exception copy live in fixed static
 * storage (1024 threads, a 64 KiB context buffer);
 * the only new resource is the writer thread. If that thread has not started within
 * kWriterStartTimeoutMs (for example because the requester holds the loader lock) the write fails
 * closed with ERROR_TIMEOUT and no dump; there is no same-thread fallback. Once started, the
 * writer is waited for until it finishes. Not reentrant: callers serialize through
 * Spark::StackTrace::SymbolLockLease, which every MiniDumpWriteDump caller already holds because
 * DbgHelp is process-global.
 */
#pragma once

#ifdef _WIN32

// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on

namespace Spark::CrashDump
{
    /**
     * @brief Write a minidump of the current process to @p file without any thread-stack memory.
     * @param file Open, writable, empty file handle (left open).
     * @param type MINIDUMP_TYPE flags; must not request full or private read-write memory.
     * @param exception Exception information, or nullptr.
     * @param error Receives the failure code (ERROR_SUCCESS on success); may be nullptr.
     * @return TRUE on success. With more than 1024 threads not every stack can be removed, so the
     *         file is truncated to zero bytes and FALSE is returned (ERROR_BUFFER_OVERFLOW).
     */
    BOOL WriteWithoutStacks(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* exception, DWORD* error);

    /// How long the requester waits for the writer thread to start before failing closed.
    inline constexpr DWORD kWriterStartTimeoutMs = 10000;

    /**
     * @brief Write a minidump of the current process from a dedicated writer thread.
     * @param removeStacks true removes every thread's stack as WriteWithoutStacks does; false
     *        keeps DbgHelp's default stack capture (the SPARK_CRASH_FULL_DUMP opt-in).
     * @param startTimeoutMs Bound on the writer thread starting; see the header contract.
     * Other parameters and the return value are as for WriteWithoutStacks; @p error receives
     * ERROR_TIMEOUT when the writer never started.
     */
    BOOL WriteOnWriterThread(HANDLE file, MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION* exception,
                             bool removeStacks, DWORD startTimeoutMs, DWORD* error);
} // namespace Spark::CrashDump

#endif
