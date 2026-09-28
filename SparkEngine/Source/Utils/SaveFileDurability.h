/**
 * @file SaveFileDurability.h
 * @brief Durable file primitives behind the SaveSystem atomic write path and whole-document
 *        editor writes (prefabs, project files).
 *
 * Contract:
 * - Thread affinity: async-safe. The functions keep no state; concurrent calls on
 *   distinct paths are independent. Callers serialize writes to the same slot.
 * - Ownership: callers own every path; nothing is retained after a call returns.
 * - Allocation: path temporaries only. These run on save/load, never per frame.
 * - Failures are reported through the return value and @p error. Only std::bad_alloc
 *   from building a staging path can escape.
 *
 * Crash model: a process killed at any instruction inside these functions leaves every
 * destination either at its previous complete contents or at its new complete contents.
 * Only a `<destination>.tmp` staging file (or, for PublishFileAtomically, one uniquely named
 * `<destination>.*.tmp` file) can be left behind, and it is never read as a save (SaveSystem
 * lists and loads `.spark_save` and `.spark_save.bak` only).
 *
 * Link safety: staging files are created exclusively and never through a link, so a
 * symlink or hard link planted at the predictable `.tmp` name cannot redirect a write.
 */

#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace Spark::SaveFileDurability
{
    /**
     * @brief Outcome of ReplaceFileAtomically. The commit point is the rename itself.
     */
    enum class ReplaceOutcome
    {
        NotCommitted,        ///< @p destination still names its previous contents.
        CommittedNotDurable, ///< @p destination names the staged contents, but the directory sync failed.
        CommittedDurable,    ///< @p destination names the staged contents on stable storage.
    };

    /**
     * @brief Create @p staging exclusively, write @p bytes through that handle and flush them.
     *
     * The staging file is created with exclusive, no-follow semantics (POSIX
     * `O_CREAT | O_EXCL | O_NOFOLLOW`, Windows `CREATE_NEW | FILE_FLAG_OPEN_REPARSE_POINT`)
     * and verified to be a fresh regular file with a single link before anything is
     * written. A path-based truncating open would follow a symlink or hard link planted at
     * the predictable staging name and overwrite its target.
     *
     * An entry already at @p staging (a file a killed writer left behind, or a planted
     * link) is unlinked first. Unlinking removes the entry itself, never a link target. A
     * directory at @p staging is never removed and fails the call with
     * std::errc::is_a_directory.
     *
     * @param staging Staging path, normally a sibling of the destination.
     * @param bytes   Complete contents, written in binary.
     * @param error   Receives the failure reason.
     * @return true when @p staging holds exactly @p bytes on stable storage. On failure, a
     *         staging file this call created is removed.
     */
    [[nodiscard]] bool WriteStagingFile(const std::filesystem::path& staging, std::string_view bytes,
                                        std::error_code& error);

    /**
     * @brief Atomically replace @p destination with @p temporary.
     *
     * POSIX opens the parent directory before the rename, renames, and then fsyncs that
     * directory so the new name is durable. A directory that cannot be opened fails the call
     * before anything is committed. Windows uses MoveFileExW with
     * MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
     *
     * @param temporary   Complete, already flushed staging file on the same volume.
     * @param destination File to replace (created when absent).
     * @param error       Receives the OS error for NotCommitted and CommittedNotDurable.
     * @return Whether and how durably @p destination now names the staged contents. A
     *         directory sync failure after the rename is CommittedNotDurable, never
     *         NotCommitted: the new contents are already visible under @p destination.
     */
    [[nodiscard]] ReplaceOutcome ReplaceFileAtomically(const std::filesystem::path& temporary,
                                                       const std::filesystem::path& destination,
                                                       std::error_code& error);

    /**
     * @brief Copy @p source over @p destination without ever exposing a partial copy.
     *
     * The bytes are streamed into `<destination>.tmp` through an exclusive, no-follow handle
     * (see WriteStagingFile), flushed, and renamed over @p destination. An interruption at
     * any point leaves @p destination at its previous complete contents (or absent, if it
     * was absent). A plain in-place copy truncates the destination first, so a kill mid-copy
     * would destroy the previous copy.
     *
     * @param source      Complete file to copy.
     * @param destination File to replace with the copy.
     * @param error       Receives the error on failure; the staging file is removed. After a
     *                    successful commit whose directory sync failed, it holds that error.
     * @return true when @p destination now names a complete, flushed copy of @p source.
     */
    [[nodiscard]] bool CopyFileAtomically(const std::filesystem::path& source, const std::filesystem::path& destination,
                                          std::error_code& error);

    /**
     * @brief Path of the previous-good copy WriteFileAtomically retains for @p destination.
     *
     * Readers that recover from the retained copy use this so the suffix has one owner.
     *
     * @param destination Document path.
     * @return `<destination>.bak`.
     */
    [[nodiscard]] std::filesystem::path BackupPathFor(const std::filesystem::path& destination);

    /**
     * @brief Replace a whole document with @p bytes without ever exposing a partial file.
     *
     * The bytes are written to `<destination>.tmp` with WriteStagingFile and flushed. When @p retainBackup is true
     * and @p destination exists, its current contents are copied to BackupPathFor(destination)
     * with CopyFileAtomically. The staging file is then renamed over @p destination.
     *
     * On failure the staging file is removed and @p destination keeps its previous bytes. The
     * retained copy keeps its previous bytes when the failure precedes the refresh; when the
     * final rename fails after the refresh, it holds a copy of @p destination's previous bytes.
     * An interruption between the refresh and the rename leaves the same state.
     *
     * The retained copy is refreshed from whatever @p destination holds, because this function
     * does not parse documents. A caller that rejected @p destination as damaged and recovered
     * from the retained copy must pass @p retainBackup = false until a write succeeds.
     * Otherwise the refresh overwrites the only good copy with the damaged document, and a
     * failed or interrupted rename then leaves no good copy on disk.
     *
     * @param destination  Document to replace; its parent directory must exist.
     * @param bytes        Complete new contents, written in binary mode.
     * @param retainBackup Keep the previous contents in BackupPathFor(destination).
     * @param error        Cleared on entry; receives the failure reason. After a successful
     *                     commit whose POSIX directory sync failed, it holds that error:
     *                     the call returns true because @p destination already names
     *                     @p bytes, but that name may not survive a power loss.
     * @return true when @p destination now names exactly @p bytes (flushed to stable storage).
     */
    [[nodiscard]] bool WriteFileAtomically(const std::filesystem::path& destination, std::string_view bytes,
                                           bool retainBackup, std::error_code& error);

    /**
     * @brief Replace @p destination with @p bytes through an unpredictable, exclusively created
     *        staging file, for publishers whose directory may be shared with other principals
     *        (operator status files such as server and gateway health snapshots).
     *
     * The staging file is `<destination>.<random>.<counter>.tmp`, created by WriteStagingFile
     * (exclusive, no-follow: O_CREAT|O_EXCL|O_NOFOLLOW on POSIX, CREATE_NEW|FILE_FLAG_OPEN_REPARSE_POINT
     * on Windows). A file, symlink or hard link planted at the name is never followed or truncated, and
     * the random name cannot be predicted in advance. The bytes are flushed and then renamed over
     * @p destination with ReplaceFileAtomically.
     *
     * On any failure the staging file is removed and @p destination keeps its previous complete
     * contents; the destination is never deleted first. A process killed mid-write can leave one
     * uniquely named staging file behind.
     *
     * @param destination File to replace; its parent directory must exist.
     * @param bytes       Complete new contents, written in binary mode.
     * @param error       Cleared on entry; receives the failure reason. After a successful commit whose
     *                    POSIX directory sync failed, it holds that error (as for WriteFileAtomically).
     * @return true when @p destination now names exactly @p bytes.
     */
    [[nodiscard]] bool PublishFileAtomically(const std::filesystem::path& destination, std::string_view bytes,
                                             std::error_code& error);
} // namespace Spark::SaveFileDurability
