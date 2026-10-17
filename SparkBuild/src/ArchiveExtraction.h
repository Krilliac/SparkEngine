#pragma once

/**
 * @file ArchiveExtraction.h
 * @brief Platform-neutral safety checks for staged archive extraction (SparkBuildCore internal).
 *
 * Downloader extracts every verified archive into a fresh staging directory
 * created inside the destination (so the final moves never cross a volume,
 * even when the destination is a symlink or junction), proves the staged tree
 * is contained, and only then moves the top-level entries into the
 * destination without replacing anything that already exists. The helpers
 * here own the member-name policy, the ZIP central-directory listing, the
 * extraction completeness check, the containment walk, and the no-replace commit.
 */

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace SparkBuild::ArchiveExtraction
{
    /// Name rules differ between the two container formats SparkBuild accepts.
    enum class MemberSyntax
    {
        Zip, ///< APPNOTE 4.4.17: '/' is the only separator; colons and backslashes are refused.
        Tar  ///< POSIX names: colons and backslashes are ordinary characters (a backslash still never hides '..').
    };

    /**
     * @brief Check that an archive member name stays inside the extraction root.
     * @param name Member name exactly as stored in the archive.
     * @param syntax Container format; ZIP names may not contain ':' (a Windows
     *        drive or alternate data stream) or backslashes (extractors disagree on them).
     * @return false for empty names, NUL bytes, absolute paths, drive prefixes,
     *         and any '..' component (either separator).
     */
    [[nodiscard]] bool IsSafeMemberName(std::string_view name, MemberSyntax syntax);

    /**
     * @brief List ZIP member names from the central directory (single volume, ZIP64 aware).
     *
     * Every central-directory name must match the name in its local header, so
     * an extractor that trusts either copy sees the same, validated path.
     * @return false with @p error set when the archive structure is malformed.
     */
    [[nodiscard]] bool ListZipMembers(const std::filesystem::path& archive, std::vector<std::string>& names,
                                      std::string& error);

    /**
     * @brief Apply IsSafeMemberName to every listed member.
     * @return false with @p error naming the first offending member, or when the list is empty.
     */
    [[nodiscard]] bool ValidateMemberNames(const std::vector<std::string>& names, MemberSyntax syntax,
                                           std::string& error);

    /**
     * @brief Create a fresh, uniquely named staging directory inside @p destDir.
     *
     * Staging inside the destination keeps every commit rename on one volume,
     * including when @p destDir is a symlink or junction to another filesystem.
     * @param destDir Final destination (created, with its parents, if missing).
     * @param staging Receives the created staging directory.
     */
    [[nodiscard]] bool CreateStagingDirectory(const std::filesystem::path& destDir, std::filesystem::path& staging,
                                              std::string& error);

    /**
     * @brief Prove an extractor produced exactly the listed ZIP members.
     *
     * Every listed member must exist in @p staging, and every staged entry must
     * be a listed member or an ancestor directory of one. This refuses a
     * partial or reinterpreted extraction even when the extractor reported success.
     */
    [[nodiscard]] bool VerifyStagedMembers(const std::filesystem::path& staging, const std::vector<std::string>& names,
                                           std::string& error);

    /**
     * @brief Walk a staged tree and prove it is self-contained.
     *
     * Only directories, regular files and symlinks are accepted. A symlink
     * target must be relative, may climb with leading '..' components only
     * through the link's own ancestor directories inside @p staging, and must
     * then descend by plain names (no later '..'), so it can never leave the
     * tree and re-enter through the staging directory's own name. Its fully
     * resolved target must also stay inside @p staging. Dangling or looping
     * links, special files and an empty tree are rejected.
     */
    [[nodiscard]] bool ValidateStagedTree(const std::filesystem::path& staging, std::string& error);

    /**
     * @brief Move each top-level staged entry into @p destDir without replacing anything.
     *
     * If any entry already exists in the destination nothing is moved. A move
     * failure part-way through moves the already-committed entries back into
     * staging, so the destination is left as it was.
     */
    [[nodiscard]] bool CommitStagedTree(const std::filesystem::path& staging, const std::filesystem::path& destDir,
                                        std::string& error);
} // namespace SparkBuild::ArchiveExtraction
