/**
 * @file TestEditorCrashHandlerFilterReal.cpp
 * @brief Production-source test that EditorCrashHandler really installs and restores the crash filter.
 *
 * editor-core-01: EditorUI logged "Crash handler initialized successfully" while
 * nothing was installed, so every editor crash went to the default OS handler
 * with no dump or log. That is the "a check that stops
 * checking" shape: the reassuring message was printed by code that did no work.
 *
 * These tests drive SparkEditor::EditorCrashHandler (SparkEditor/Source/Core/
 * EditorCrashHandler.cpp, already linked into SparkTests) and assert against the
 * observable process state - the Windows unhandled-exception filter itself - not
 * against a log line.
 */

#include "TestFramework.h"

#include "Core/EditorCrashHandler.h"
#include "Utils/StackTrace.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <span>
#include <string>

#ifdef _WIN32
#include "Utils/MiniDumpWithoutStacks.h"

#include <dbghelp.h>
#endif

namespace
{
    /** @brief Unique scratch directory removed on scope exit. */
    class ScratchCrashDir
    {
      public:
        explicit ScratchCrashDir(const std::string& name)
        {
            m_path = std::filesystem::temp_directory_path() / ("spark_editor_crash_" + name);
            std::error_code ignored;
            std::filesystem::remove_all(m_path, ignored);
            std::filesystem::create_directories(m_path, ignored);
        }

        ~ScratchCrashDir()
        {
            std::error_code ignored;
            std::filesystem::remove_all(m_path, ignored);
        }

        ScratchCrashDir(const ScratchCrashDir&) = delete;
        ScratchCrashDir& operator=(const ScratchCrashDir&) = delete;

        std::string Path() const { return m_path.string(); }

      private:
        std::filesystem::path m_path;
    };

    std::string ReadSourceFile(const std::filesystem::path& relativePath)
    {
        const std::filesystem::path path = std::filesystem::path(SPARK_TEST_SOURCE_DIR) / relativePath;
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
} // namespace

TEST(EditorCrashHandler_RecoveryPersistenceIsNotOwnedByCrashHandler)
{
    const std::string source = ReadSourceFile("SparkEditor/Source/Core/EditorCrashHandler.cpp");
    const std::string header = ReadSourceFile("SparkEditor/Source/Core/EditorCrashHandler.h");
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(header.empty());

    EXPECT_FALSE(source.contains("SaveRecoveryData("));
    EXPECT_FALSE(source.contains("m_recoveryCallback"));
    EXPECT_FALSE(source.contains("AutoSaveRecoveryThread"));
    EXPECT_FALSE(header.contains("RecoveryData"));
    EXPECT_FALSE(header.contains("SetRecoveryCallback"));
}

TEST(EditorCrashHandler_RecordOperationStillAcceptsBoundedHistory)
{
    ScratchCrashDir scratch("operations");
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();
    ASSERT_TRUE(handler.Initialize(scratch.Path()));
    for (int index = 0; index != 75; ++index)
        handler.RecordOperation("op-" + std::to_string(index));
    handler.Shutdown();
    EXPECT_TRUE(true);
}

#ifdef _WIN32

namespace
{
    /** @brief Read the currently installed top-level filter without changing it. */
    LPTOP_LEVEL_EXCEPTION_FILTER CurrentUnhandledExceptionFilter()
    {
        LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(nullptr);
        SetUnhandledExceptionFilter(current);
        return current;
    }

    /** @brief Memory ranges held in the MemoryListStream of the minidump image @p dump. */
    std::span<const MINIDUMP_MEMORY_DESCRIPTOR> DumpMemoryRanges(const std::string& dump)
    {
        void* stream = nullptr;
        ULONG streamSize = 0;
        if (!MiniDumpReadDumpStream(const_cast<char*>(dump.data()), MemoryListStream, nullptr, &stream, &streamSize) ||
            streamSize < sizeof(ULONG32))
            return {};
        const auto* list = static_cast<const MINIDUMP_MEMORY_LIST*>(stream);
        return {list->MemoryRanges, list->NumberOfMemoryRanges};
    }

    /** @brief Failure diagnostic: which part of the minidump image @p dump holds file offset @p offset. */
    std::string DumpRegionHolding(const std::string& dump, size_t offset)
    {
        const auto within = [offset](ULONG64 rva, ULONG64 size) { return offset >= rva && offset - rva < size; };
        MINIDUMP_HEADER header{};
        if (dump.size() < sizeof(header))
            return "no minidump header";
        std::memcpy(&header, dump.data(), sizeof(header));
        if (within(0, sizeof(header)))
            return "the minidump header";
        const ULONG64 directorySize = ULONG64{header.NumberOfStreams} * sizeof(MINIDUMP_DIRECTORY);
        if (header.StreamDirectoryRva + directorySize > dump.size())
            return "a truncated stream directory";
        if (within(header.StreamDirectoryRva, directorySize))
            return "the stream directory";
        std::string owner;
        for (ULONG32 index = 0; index != header.NumberOfStreams; ++index)
        {
            MINIDUMP_DIRECTORY entry{};
            std::memcpy(&entry, dump.data() + header.StreamDirectoryRva + index * sizeof(entry), sizeof(entry));
            if (within(entry.Location.Rva, entry.Location.DataSize))
                owner += std::format("stream type {} [0x{:x}, +0x{:x}) ", entry.StreamType, entry.Location.Rva,
                                     entry.Location.DataSize);
        }
        // Thread contexts live outside the ThreadListStream's own bytes.
        void* stream = nullptr;
        ULONG streamSize = 0;
        if (MiniDumpReadDumpStream(const_cast<char*>(dump.data()), ThreadListStream, nullptr, &stream, &streamSize) &&
            streamSize >= sizeof(ULONG32))
        {
            const auto* threads = static_cast<const MINIDUMP_THREAD_LIST*>(stream);
            for (ULONG32 index = 0; index != threads->NumberOfThreads; ++index)
            {
                const MINIDUMP_THREAD& thread = threads->Threads[index];
                if (within(thread.ThreadContext.Rva, thread.ThreadContext.DataSize))
                    owner += std::format("context of thread {} ", thread.ThreadId);
            }
        }
        return owner.empty() ? std::string("no stream or thread context (indirect data or file slack)") : owner;
    }

    constexpr bool IsPartialCopy(DWORD error)
    {
        return error == ERROR_PARTIAL_COPY || error == static_cast<DWORD>(HRESULT_FROM_WIN32(ERROR_PARTIAL_COPY));
    }
} // namespace

TEST(EditorCrashHandlerReal_InitializeInstallsAnUnhandledExceptionFilter)
{
    ScratchCrashDir scratch("install");
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();

    const LPTOP_LEVEL_EXCEPTION_FILTER before = CurrentUnhandledExceptionFilter();

    ASSERT_TRUE(handler.Initialize(scratch.Path()));
    const LPTOP_LEVEL_EXCEPTION_FILTER installed = CurrentUnhandledExceptionFilter();
    // The whole crash path hangs off this pointer; if it is unchanged the
    // handler reported success while doing nothing.
    EXPECT_TRUE(installed != before);
    EXPECT_TRUE(installed != nullptr);

    handler.Shutdown();
    // Shutdown must hand the process back exactly as it found it, or a later
    // owner (the engine crash handler, a debugger) loses its filter.
    EXPECT_TRUE(CurrentUnhandledExceptionFilter() == before);
}

TEST(EditorCrashHandlerReal_SymbolBusyWritesAnUnsymbolizedReport)
{
    ScratchCrashDir scratch("symbol_busy");
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();
    ASSERT_TRUE(handler.Initialize(scratch.Path()));

    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = STATUS_FATAL_APP_EXIT;
    EXCEPTION_POINTERS pointers{&record, &context};
    int callbackCount = 0;
    const auto filter = CurrentUnhandledExceptionFilter();
    ASSERT_TRUE(filter != nullptr);
    handler.SetCrashCallback(
        [&](const SparkEditor::CrashInfo&)
        {
            ++callbackCount;
            EXPECT_EQ(filter(&pointers), EXCEPTION_EXECUTE_HANDLER);
        });

    {
        // Invoke the installed production filter while this same thread owns
        // DbgHelp. It must preserve a report without recursive symbol calls
        // or writing a partial full-memory dump.
        Spark::StackTrace::SymbolLockLease symbolLock;
        ASSERT_TRUE(symbolLock.owns_lock());
        EXPECT_EQ(filter(&pointers), EXCEPTION_EXECUTE_HANDLER);
    }
    handler.SetCrashCallback({});
    handler.Shutdown();

    bool foundReport = false;
    bool foundDump = false;
    for (const auto& entry : std::filesystem::directory_iterator(scratch.Path()))
    {
        if (entry.path().extension() == ".dmp")
            foundDump = true;
        if (entry.path().extension() != ".log")
            continue;
        std::ifstream report(entry.path());
        const std::string contents{std::istreambuf_iterator<char>(report), std::istreambuf_iterator<char>()};
        foundReport |= contents.find("DbgHelp busy") != std::string::npos;
    }
    EXPECT_TRUE(foundReport);
    EXPECT_FALSE(foundDump);
    EXPECT_EQ(callbackCount, 1);
}

TEST(EditorCrashHandlerReal_InitializeCreatesTheCrashDirectory)
{
    ScratchCrashDir scratch("mkdir");
    const std::string nested = (std::filesystem::path(scratch.Path()) / "nested" / "Crashes").string();
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();

    ASSERT_TRUE(handler.Initialize(nested));
    EXPECT_TRUE(std::filesystem::is_directory(nested));
    handler.Shutdown();
}

TEST(EditorCrashHandlerReal_InitializeRejectsAnEmptyDirectoryInsteadOfInstalling)
{
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();
    const LPTOP_LEVEL_EXCEPTION_FILTER before = CurrentUnhandledExceptionFilter();

    EXPECT_FALSE(handler.Initialize(""));
    // A refused initialization must not leave a filter behind.
    EXPECT_TRUE(CurrentUnhandledExceptionFilter() == before);
}

TEST(EditorCrashHandlerReal_ShutdownWithoutInitializeLeavesTheFilterAlone)
{
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();
    const LPTOP_LEVEL_EXCEPTION_FILTER before = CurrentUnhandledExceptionFilter();

    handler.Shutdown();
    EXPECT_TRUE(CurrentUnhandledExceptionFilter() == before);
}

// OPS-100: an editor dump must not carry the whole heap (tokens, credentials,
// clipboard) into a file users are asked to share.
TEST(EditorCrashHandler_DefaultDumpExcludesFullMemory)
{
    const std::uint32_t dumpType = SparkEditor::EditorCrashHandler::CrashDumpType();
    EXPECT_EQ(dumpType & static_cast<std::uint32_t>(MiniDumpWithFullMemory), 0u);
    EXPECT_EQ(dumpType & static_cast<std::uint32_t>(MiniDumpWithPrivateReadWriteMemory), 0u);
    EXPECT_EQ(dumpType & static_cast<std::uint32_t>(MiniDumpWithIndirectlyReferencedMemory), 0u);
    EXPECT_EQ(dumpType & static_cast<std::uint32_t>(MiniDumpWithHandleData), 0u);
    EXPECT_NE(dumpType & static_cast<std::uint32_t>(MiniDumpWithThreadInfo), 0u);

    // The writer must use that selection, not a hard-coded full-memory type, and must go through
    // the stack-removing writer rather than a raw MiniDumpWriteDump that copies every stack.
    const std::string source = ReadSourceFile("SparkEditor/Source/Core/EditorCrashHandler.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_TRUE(source.contains("static_cast<MINIDUMP_TYPE>(CrashDumpType())"));
    EXPECT_FALSE(source.contains("MiniDumpWithFullMemory"));
    EXPECT_TRUE(
        source.contains("Spark::CrashDump::WriteWithoutStacks(hFile, static_cast<MINIDUMP_TYPE>(CrashDumpType())"));
    EXPECT_FALSE(source.contains("MiniDumpWriteDump("));
}

// OPS-100: a secret live on the dumping thread's stack must not reach an editor dump. The control
// dump (same type, raw MiniDumpWriteDump) must contain it, which proves the scan can see a leak.
TEST(EditorCrashHandler_DumpTypeWritesNoStackResidentSecret)
{
    // Built at run time, so the image's read-only data holds no copy: the only ones are this stack
    // array and the string's heap buffer.
    const std::string canary = std::format("SPARKEDITORCANARY-{:016x}", std::random_device{}() * 0x9E3779B97F4A7C15ull);
    volatile char stackSecret[64] = {};
    for (size_t index = 0; index < canary.size(); ++index)
        stackSecret[index] = canary[index];

    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    EXCEPTION_POINTERS pointers{&record, &context};
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), &pointers, FALSE};
    const auto dumpType = static_cast<MINIDUMP_TYPE>(SparkEditor::EditorCrashHandler::CrashDumpType());

    ScratchCrashDir dir("stack_canary");
    const auto writeDump = [&](const std::filesystem::path& file, bool removeStacks) -> std::string
    {
        HANDLE handle =
            CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return {};
        BOOL written = FALSE;
        DWORD error = ERROR_SUCCESS;
        {
            Spark::StackTrace::SymbolLockLease symbolLock(true);
            if (symbolLock.owns_lock())
            {
                // The raw control only proves the scan can see the canary; like the product writer it
                // retries once when DbgHelp races a transiently unreadable page.
                for (int attempt = 0; attempt != 2; ++attempt)
                {
                    LARGE_INTEGER start{};
                    SetFilePointerEx(handle, start, nullptr, FILE_BEGIN);
                    SetEndOfFile(handle);
                    SetLastError(ERROR_SUCCESS);
                    written = removeStacks ? Spark::CrashDump::WriteWithoutStacks(handle, dumpType, &exception, &error)
                                           : MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), handle,
                                                               dumpType, &exception, nullptr, nullptr);
                    if (!removeStacks)
                        error = written ? ERROR_SUCCESS : GetLastError();
                    if (written || removeStacks || !IsPartialCopy(error))
                        break;
                }
            }
            else
            {
                error = ERROR_BUSY;
            }
        }
        CloseHandle(handle);
        if (!written)
        {
            std::printf("  %s dump failed: error 0x%lx\n", removeStacks ? "filtered" : "control", error);
            return {};
        }
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    const std::string control = writeDump(std::filesystem::path(dir.Path()) / "control.dmp", false);
    ASSERT_FALSE(control.empty());
    EXPECT_TRUE(control.find(canary) != std::string::npos);

    const std::string filtered = writeDump(std::filesystem::path(dir.Path()) / "editor.dmp", true);
    ASSERT_FALSE(filtered.empty());

    // No byte of this thread's stack reservation may reach the dump, whichever stack pointer DbgHelp
    // copied from and whatever stale frames lie below it.
    ULONG_PTR stackLow = 0;
    ULONG_PTR stackHigh = 0;
    GetCurrentThreadStackLimits(&stackLow, &stackHigh);
    const auto ranges = DumpMemoryRanges(filtered);
    ASSERT_FALSE(ranges.empty());
    for (const MINIDUMP_MEMORY_DESCRIPTOR& range : ranges)
    {
        const ULONG64 start = range.StartOfMemoryRange;
        const ULONG64 end = start + range.Memory.DataSize;
        const bool overlapsStack = start < stackHigh && end > stackLow;
        if (overlapsStack)
            std::printf("  dump range [0x%llx, 0x%llx) overlaps this thread's stack [0x%llx, 0x%llx)\n", start, end,
                        static_cast<ULONG64>(stackLow), static_cast<ULONG64>(stackHigh));
        EXPECT_FALSE(overlapsStack);
    }

    // On failure, name the address the secret came from so another DbgHelp build is diagnosable.
    const size_t leak = filtered.find(canary);
    for (const MINIDUMP_MEMORY_DESCRIPTOR& range : ranges)
    {
        if (leak != std::string::npos && leak >= range.Memory.Rva && leak - range.Memory.Rva < range.Memory.DataSize)
            std::printf("  canary at dump offset 0x%zx = address 0x%llx\n", leak,
                        range.StartOfMemoryRange + (leak - range.Memory.Rva));
    }
    if (leak != std::string::npos)
        std::printf("  canary at dump offset 0x%zx lies in %s\n", leak, DumpRegionHolding(filtered, leak).c_str());
    EXPECT_TRUE(leak == std::string::npos);
    EXPECT_EQ(stackSecret[0], 'S');
}

// The dump writer runs DbgHelp on its own thread. If that thread cannot start (here: this thread
// holds the loader lock, so the writer blocks before DLL_THREAD_ATTACH), the write must fail closed
// within the bound instead of hanging, write nothing, and never fall back to this thread. The late
// writer must not hijack the next request either.
TEST(CrashDumpWriter_BlockedWriterThreadFailsClosedWithinTheBound)
{
    using LockLoaderLock = LONG(NTAPI*)(ULONG, ULONG*, PVOID*);
    using UnlockLoaderLock = LONG(NTAPI*)(ULONG, PVOID);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    ASSERT_TRUE(ntdll != nullptr);
    const auto lockLoader = reinterpret_cast<LockLoaderLock>(GetProcAddress(ntdll, "LdrLockLoaderLock"));
    const auto unlockLoader = reinterpret_cast<UnlockLoaderLock>(GetProcAddress(ntdll, "LdrUnlockLoaderLock"));
    ASSERT_TRUE(lockLoader != nullptr && unlockLoader != nullptr);

    CONTEXT context{};
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record{};
    record.ExceptionCode = EXCEPTION_BREAKPOINT;
    EXCEPTION_POINTERS pointers{&record, &context};
    MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), &pointers, FALSE};
    const auto dumpType = static_cast<MINIDUMP_TYPE>(SparkEditor::EditorCrashHandler::CrashDumpType());

    ScratchCrashDir dir("writer_timeout");
    const std::filesystem::path blockedPath = std::filesystem::path(dir.Path()) / "blocked.dmp";
    const std::filesystem::path laterPath = std::filesystem::path(dir.Path()) / "later.dmp";
    HANDLE blocked =
        CreateFileW(blockedPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    HANDLE later = CreateFileW(laterPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    ASSERT_TRUE(blocked != INVALID_HANDLE_VALUE && later != INVALID_HANDLE_VALUE);

    BOOL blockedWritten = TRUE;
    DWORD blockedError = ERROR_SUCCESS;
    ULONGLONG elapsedMs = 0;
    BOOL laterWritten = FALSE;
    DWORD laterError = ERROR_SUCCESS;
    {
        Spark::StackTrace::SymbolLockLease symbolLock(true);
        ASSERT_TRUE(symbolLock.owns_lock());

        PVOID cookie = nullptr;
        ASSERT_EQ(lockLoader(0, nullptr, &cookie), 0);
        const ULONGLONG started = GetTickCount64();
        blockedWritten = Spark::CrashDump::WriteOnWriterThread(blocked, dumpType, &exception, true, 500, &blockedError);
        elapsedMs = GetTickCount64() - started;
        unlockLoader(0, cookie);

        laterWritten = Spark::CrashDump::WriteWithoutStacks(later, dumpType, &exception, &laterError);
    }
    // The abandoned writer has been free to start since the unlock; give it time to (wrongly) write.
    Sleep(200);
    CloseHandle(blocked);
    CloseHandle(later);

    EXPECT_FALSE(blockedWritten);
    EXPECT_EQ(blockedError, static_cast<DWORD>(ERROR_TIMEOUT));
    EXPECT_TRUE(elapsedMs < 5000);
    std::error_code ignored;
    EXPECT_EQ(std::filesystem::file_size(blockedPath, ignored), static_cast<std::uintmax_t>(0));
    EXPECT_TRUE(laterWritten);
    EXPECT_EQ(laterError, static_cast<DWORD>(ERROR_SUCCESS));
    EXPECT_TRUE(std::filesystem::file_size(laterPath, ignored) > 0);
}

#else

TEST(EditorCrashHandlerReal_InitializeCreatesTheCrashDirectory)
{
    ScratchCrashDir scratch("mkdir");
    const std::string nested = (std::filesystem::path(scratch.Path()) / "nested" / "Crashes").string();
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();

    ASSERT_TRUE(handler.Initialize(nested));
    EXPECT_TRUE(std::filesystem::is_directory(nested));
    handler.Shutdown();
}

TEST(EditorCrashHandlerReal_InitializeRejectsAnEmptyDirectory)
{
    SparkEditor::EditorCrashHandler& handler = SparkEditor::EditorCrashHandler::GetInstance();
    EXPECT_FALSE(handler.Initialize(""));
}

#endif
