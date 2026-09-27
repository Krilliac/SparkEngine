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
 * Only a `<destination>.tmp` staging file can be left behind, and it is never read as a
 * save (SaveSystem lists and loads `.spark_save` and `.spark_save.bak` only).
 */

#pragma once

#include <filesystem>
#include <string_view>
#include <system_error>

namespace Spark::SaveFileDurability
{
    /**
     * @brief Flush @p path's contents to stable storage (fsync / FlushFileBuffers).
     * @param path  Existing regular file.
     * @param error Receives the OS error on failure.
     * @return true when the flush succeeded.
     */
    [[nodiscard]] bool FlushFileDurably(const std::filesystem::path& path, std::error_code& error);

    /**
     * @brief Atomically replace @p destination with @p temporary.
     *
     * POSIX renames and then fsyncs the parent directory so the new name is durable;
     * Windows uses MoveFileExW with MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH.
     *
     * @param temporary   Complete, already flushed staging file on the same volume.
     * @param destination File to replace (created when absent).
     * @param error       Receives the OS error on failure.
     * @return true when @p destination now names the staged contents.
     */
    [[nodiscard]] bool ReplaceFileAtomically(const std::filesystem::path& temporary,
                                             const std::filesystem::path& destination, std::error_code& error);

    /**
     * @brief Copy @p source over @p destination without ever exposing a partial copy.
     *
     * The bytes are staged in `<destination>.tmp`, flushed, and renamed over
     * @p destination. An interruption at any point leaves @p destination at its previous
     * complete contents (or absent, if it was absent). A plain in-place copy truncates the
     * destination first, so a kill mid-copy would destroy the previous copy.
     *
     * @param source      Complete file to copy.
     * @param destination File to replace with the copy.
     * @param error       Receives the error on failure; the staging file is removed.
     * @return true when @p destination now holds a complete, flushed copy of @p source.
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
     * The bytes are written to `<destination>.tmp` and flushed. When @p retainBackup is true
     * and @p destination exists, its current contents are copied to BackupPathFor(destination)
     * with CopyFileAtomically. The staging file is then renamed over @p destination.
     *
     * On failure the staging file is removed, and @p destination and its retained copy keep
     * their previous bytes. The retained copy is refreshed from whatever @p destination holds;
     * this function does not parse documents, so a reader that rejects a damaged primary should
     * recover from the retained copy before the next write refreshes it.
     *
     * @param destination  Document to replace; its parent directory must exist.
     * @param bytes        Complete new contents, written in binary mode.
     * @param retainBackup Keep the previous contents in BackupPathFor(destination).
     * @param error        Cleared on entry; receives the failure reason.
     * @return true when @p destination now holds exactly @p bytes on stable storage.
     */
    [[nodiscard]] bool WriteFileAtomically(const std::filesystem::path& destination, std::string_view bytes,
                                           bool retainBackup, std::error_code& error);
} // namespace Spark::SaveFileDurability
