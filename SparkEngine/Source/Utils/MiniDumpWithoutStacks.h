/**
 * @file MiniDumpWithoutStacks.h
 * @brief Minidump writer that omits every thread's stack memory (OPS-100).
 *
 * Thread stacks hold live locals (passwords, SCRAM keys, session tokens). MiniDumpNormal copies
 * them, MiniDumpFilterMemory still copies them verbatim, and DbgHelp ignores a cleared
 * ThreadWriteStack flag, so the only reliable way to keep them out is to hand every thread's
 * stack back through RemoveMemoryCallback. The whole reservation goes (from its allocation base to
 * StackBase), so neither the stack pointer DbgHelp picks for the dumping thread nor stale frames
 * below it can put stack bytes in the file. Registers, the instruction window, the module and
 * thread lists and the exception record stay in the dump.
 *
 * Contract: Windows only. Crash-path safe: no allocation (fixed storage for 1024 threads).
 * Not reentrant: callers serialize through Spark::StackTrace::SymbolLockLease, which every
 * MiniDumpWriteDump caller already holds because DbgHelp is process-global.
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
} // namespace Spark::CrashDump

#endif
