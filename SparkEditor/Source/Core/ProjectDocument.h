/**
 * @file ProjectDocument.h
 * @brief Reader and writer for .sparkproject documents and the recent-projects list
 *
 * ProjectManager reads a project document (and its .bak) and RecentProjects.json from
 * disk and hands the bytes here; SaveProjectFile and SaveRecentProjectsList write what
 * these writers produce. The grammar is a field scrape over a JSON object, not a full JSON
 * parser: a key is a quoted name followed by ':', a string value is the next string
 * literal, and arrays are scanned string-aware. It lives in its own translation unit so
 * the SEC-120 fuzz target links the shipped reader without ProjectManager's project,
 * template and scene machinery.
 *
 * Thread affinity: any thread; no shared state.
 * Ownership: inputs are borrowed; results are returned by value.
 * Allocation: the extracted strings and arrays, bounded by the caller's file-size limits
 * (4 MiB for a project document, 1 MiB for the recent-projects list).
 */

#pragma once

#include "ProjectManager.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace SparkEditor
{

    /// @brief Verdict of the structural and version gate on a project document.
    enum class ProjectDocumentStatus : std::uint8_t
    {
        Ok,
        Rejected,
        NewerVersion
    };

    /// @brief The fields LoadProjectFile reads from a project document.
    struct ProjectDocumentFields
    {
        std::string name;
        std::string version;
        std::string description;
        std::string engineVersion;
        std::string templateId; ///< "template" stable identity; empty when the document has none.
        std::string defaultScene;
        std::string lastOpenedScene;
        uint64_t lastModified = 0; ///< 0 when absent.
        uint64_t createdTime = 0;  ///< 0 when absent.
        std::vector<std::string> scenes;
        std::vector<std::string> modules;

        bool operator==(const ProjectDocumentFields&) const = default;
    };

    /// @brief Most entries the recent-projects list keeps (the UI never shows more).
    inline constexpr size_t kMaxRecentProjects = 15;

    /// @brief Escape a string for a JSON string literal (quotes, backslash, control bytes).
    std::string EscapeProjectJsonString(const std::string& s);

    /**
     * @brief Structural and version gate for a project document.
     *
     * Every writer emits one complete JSON object, so a missing brace at either end means
     * a truncated or damaged file. A document without projectFileVersion is the legacy
     * dialect (hand-written and spark.project.json documents) and reads as version 1.
     * @param reason Receives the rejection reason.
     */
    ProjectDocumentStatus CheckProjectDocument(const std::string& content, std::string& reason);

    /**
     * @brief Extract the project fields from a document CheckProjectDocument accepted.
     *
     * A missing field reads as empty or 0. A timestamp that does not fit 64 bits makes the
     * document unreadable (false, with @p reason), so the loader falls back to the .bak.
     */
    bool ReadProjectDocumentFields(const std::string& content, ProjectDocumentFields& fields, std::string& reason);

    /// @brief Render @p fields as the project document SaveProjectFile writes.
    std::string WriteProjectDocument(const ProjectDocumentFields& fields);

    /**
     * @brief Extract every entry with a non-empty path from a recent-projects document.
     *
     * An entry whose lastOpened does not fit 64 bits is dropped. Paths are returned as
     * stored; ProjectManager normalizes them and keeps the first kMaxRecentProjects that it
     * can resolve.
     */
    std::vector<RecentProject> ReadRecentProjectsDocument(const std::string& content);

    /// @brief Render the first kMaxRecentProjects entries as RecentProjects.json.
    std::string WriteRecentProjectsDocument(std::span<const RecentProject> projects);

} // namespace SparkEditor
