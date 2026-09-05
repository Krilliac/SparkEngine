/**
 * @file TestEditorCrashHandlerFilterReal.cpp
 * @brief Production-source test that EditorCrashHandler really installs and restores the crash filter.
 *
 * editor-core-01: EditorUI logged "Crash handler initialized successfully" while
 * nothing was installed, so every editor crash went to the default OS handler
 * with no dump, no log and no recovery.json. That is the "a check that stops
 * checking" shape: the reassuring message was printed by code that did no work.
 *
 * These tests drive SparkEditor::EditorCrashHandler (SparkEditor/Source/Core/
 * EditorCrashHandler.cpp, already linked into SparkTests) and assert against the
 * observable process state - the Windows unhandled-exception filter itself - not
 * against a log line.
 */

#include "TestFramework.h"

#include "Core/EditorCrashHandler.h"

#include <filesystem>
#include <string>

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
} // namespace

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
