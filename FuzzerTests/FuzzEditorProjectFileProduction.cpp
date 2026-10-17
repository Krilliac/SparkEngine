/**
 * @file FuzzEditorProjectFileProduction.cpp
 * @brief libc++-compiled production adapter for the editor project-file libFuzzer harness.
 *
 * ProjectManager::LoadProjectFile reads a .sparkproject (or its .bak) through one bounded
 * handle and hands the bytes to CheckProjectDocument and ReadProjectDocumentFields;
 * LoadRecentProjectsList hands RecentProjects.json to ReadRecentProjectsDocument. The
 * adapter reads the fuzz bytes both ways. A violated contract aborts so libFuzzer records a
 * crash:
 *  - nothing throws (std::stoull used to throw out of the document read);
 *  - a refused document or field read gives a reason, and a refused field read leaves no
 *    partially read field behind;
 *  - an accepted project document written back by WriteProjectDocument (the bytes
 *    SaveProjectFile writes) passes the gate and reads back as the identical fields, so a
 *    load/save cycle loses no scene, module or string;
 *  - every recent-projects entry has a path, and the list written back by
 *    WriteRecentProjectsDocument reads back as the identical first 15 entries.
 */

#include "FuzzEditorProjectFileProduction.h"

#include "Core/ProjectDocument.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace
{
    constexpr std::size_t kMaxInputBytes = 64u * 1024u;

    [[noreturn]] void InvariantFailure(const char* what)
    {
        std::fprintf(stderr, "SparkFuzzEditorProjectFile: project document reader violated: %s\n", what);
        std::abort();
    }

    bool SameEntry(const SparkEditor::RecentProject& a, const SparkEditor::RecentProject& b)
    {
        return a.name == b.name && a.path == b.path && a.engineVersion == b.engineVersion &&
               a.lastOpened == b.lastOpened;
    }

    void CheckDocument(const std::string& content)
    {
        using namespace SparkEditor;
        std::string reason;
        const ProjectDocumentStatus status = SparkEditor::CheckProjectDocument(content, reason);
        if (status != ProjectDocumentStatus::Ok)
        {
            if (reason.empty())
            {
                InvariantFailure("a refused project document gives no reason");
            }
            return;
        }

        ProjectDocumentFields fields;
        std::string fieldReason;
        if (!ReadProjectDocumentFields(content, fields, fieldReason))
        {
            if (fieldReason.empty())
            {
                InvariantFailure("a refused field read gives no reason");
            }
            if (!(fields == ProjectDocumentFields{}))
            {
                InvariantFailure("a refused field read left fields behind");
            }
            return;
        }

        const std::string written = WriteProjectDocument(fields);
        std::string writtenReason;
        if (SparkEditor::CheckProjectDocument(written, writtenReason) != ProjectDocumentStatus::Ok)
        {
            InvariantFailure("the document WriteProjectDocument renders is refused by the gate");
        }
        ProjectDocumentFields reread;
        std::string rereadReason;
        if (!ReadProjectDocumentFields(written, reread, rereadReason))
        {
            InvariantFailure("the document WriteProjectDocument renders cannot be read");
        }
        if (!(reread == fields))
        {
            InvariantFailure("write -> read changed the project fields");
        }
    }

    void CheckRecentList(const std::string& content)
    {
        using namespace SparkEditor;
        const std::vector<RecentProject> entries = ReadRecentProjectsDocument(content);
        for (const RecentProject& entry : entries)
        {
            if (entry.path.empty())
            {
                InvariantFailure("a recent-projects entry without a path was kept");
            }
        }
        const std::vector<RecentProject> reread = ReadRecentProjectsDocument(WriteRecentProjectsDocument(entries));
        // ProjectManager applies the same cap after parsing; the writer is also required to
        // emit only the first kMaxRecentProjects entries, so compare exactly the retained prefix.
        const std::size_t kept = std::min(entries.size(), kMaxRecentProjects);
        if (reread.size() != kept)
        {
            InvariantFailure("write -> read changed the number of recent projects");
        }
        for (std::size_t i = 0; i < kept; ++i)
        {
            if (!SameEntry(entries[i], reread[i]))
            {
                InvariantFailure("write -> read changed a recent-projects entry");
            }
        }
    }
} // namespace

extern "C" int SparkFuzzReadEditorProjectFile(const std::uint8_t* data, std::size_t size)
{
    if (size > kMaxInputBytes || (data == nullptr && size != 0))
    {
        return 0;
    }
    const std::string content = size == 0 ? std::string() : std::string(reinterpret_cast<const char*>(data), size);
    try
    {
        CheckDocument(content);
        CheckRecentList(content);
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "SparkFuzzEditorProjectFile: exception: %s\n", error.what());
        InvariantFailure("the reader threw");
    }
    return 0;
}
