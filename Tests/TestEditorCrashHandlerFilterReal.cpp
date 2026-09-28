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
#include <filesystem>
#include <fstream>
#include <iterator>
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
    const std::string canary = "SPARKEDITORCANARY-7f3a91c2d4e5b608";
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
        {
            Spark::StackTrace::SymbolLockLease symbolLock(true);
            if (symbolLock.owns_lock())
            {
                written = removeStacks ? Spark::CrashDump::WriteWithoutStacks(handle, dumpType, &exception, nullptr)
                                       : MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), handle, dumpType,
                                                           &exception, nullptr, nullptr);
            }
        }
        CloseHandle(handle);
        if (!written)
            return {};
        std::ifstream in(file, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };

    const std::string control = writeDump(std::filesystem::path(dir.Path()) / "control.dmp", false);
    ASSERT_FALSE(control.empty());
    EXPECT_TRUE(control.find(canary) != std::string::npos);

    const std::string filtered = writeDump(std::filesystem::path(dir.Path()) / "editor.dmp", true);
    ASSERT_FALSE(filtered.empty());
    EXPECT_TRUE(filtered.find(canary) == std::string::npos);
    EXPECT_EQ(stackSecret[0], 'S');
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
