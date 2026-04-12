/**
 * @file VersionControlEnums.h
 * @brief Enumerations for the version control system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to version control integration,
 * including VCS types, file status, branch operations, and merge conflict types.
 */

#pragma once

// The Windows SDK (winnt.h) defines `DELETE` as `(0x00010000L)` and a few
// other all-caps tokens as macros. If any translation unit includes a
// Windows header before this file, the bare `DELETE` / `IN` / `OUT` /
// `OPTIONAL` tokens inside enum class bodies below would be
// preprocessor-expanded and produce confusing syntax errors. Undefine
// the troublesome macros defensively so the enums below are always
// parsed as written.
#ifdef DELETE
#undef DELETE
#endif
#ifdef IN
#undef IN
#endif
#ifdef OUT
#undef OUT
#endif
#ifdef OPTIONAL
#undef OPTIONAL
#endif
#ifdef ERROR
#undef ERROR
#endif

namespace SparkEditor
{

    /**
 * @brief Version control system types
 */
    enum class VCSType
    {
        NONE = 0,     ///< No version control
        GIT = 1,      ///< Git version control
        PERFORCE = 2, ///< Perforce version control
        SVN = 3,      ///< Subversion version control
        CUSTOM = 4    ///< Custom version control system
    };

    /**
 * @brief File status in version control
 */
    enum class FileStatus
    {
        UNTRACKED = 0,  ///< File not tracked by VCS
        ADDED = 1,      ///< File added to VCS
        MODIFIED = 2,   ///< File modified since last commit
        DELETED = 3,    ///< File deleted
        RENAMED = 4,    ///< File renamed
        COPIED = 5,     ///< File copied
        IGNORED = 6,    ///< File ignored by VCS
        CONFLICTED = 7, ///< File has merge conflicts
        LOCKED = 8,     ///< File locked by another user
        UP_TO_DATE = 9  ///< File up to date
    };

    /**
 * @brief Merge conflict types
 */
    enum class ConflictType
    {
        CONTENT_CONFLICT = 0, ///< Content conflict
        RENAME_CONFLICT = 1,  ///< Rename conflict
        DELETE_CONFLICT = 2,  ///< Delete conflict
        BINARY_CONFLICT = 3,  ///< Binary conflict
        ASSET_CONFLICT = 4,   ///< Asset conflict
        METADATA_CONFLICT = 5 ///< Metadata conflict
    };

    /**
 * @brief VCS operation types
 */
    enum class VCSOperationType
    {
        CLONE = 0,    ///< Clone repository
        PULL = 1,     ///< Pull changes
        PUSH = 2,     ///< Push changes
        COMMIT = 3,   ///< Commit changes
        MERGE = 4,    ///< Merge branches
        CHECKOUT = 5, ///< Checkout branch/commit
        FETCH = 6,    ///< Fetch changes
        STATUS = 7,   ///< Get status
        LOG = 8,      ///< Get log
        DIFF = 9,     ///< Get differences
        CUSTOM = 10   ///< Custom operation
    };

    /**
 * @brief Merge strategy types
 */
    enum class MergeStrategy
    {
        MANUAL = 0,        ///< Manual merge resolution
        AUTO_MERGE = 1,    ///< Automatic merge
        PREFER_LOCAL = 2,  ///< Prefer local changes
        PREFER_REMOTE = 3, ///< Prefer remote changes
        SMART_MERGE = 4    ///< Smart merge with conflict detection
    };

    /**
 * @brief Branch operation types
 *
 * Note: `DELETE` collides with the Windows SDK `DELETE` macro (winnt.h),
 * so we use `DELETE_BRANCH` instead. Similarly `CREATE` / `SWITCH` are
 * not Windows macros currently but renaming them for consistency.
 */
    enum class BranchOperation
    {
        CREATE_BRANCH = 0, ///< Create new branch
        DELETE_BRANCH = 1, ///< Delete branch
        RENAME_BRANCH = 2, ///< Rename branch
        MERGE_BRANCH = 3,  ///< Merge branch
        REBASE_BRANCH = 4, ///< Rebase branch
        SWITCH_BRANCH = 5  ///< Switch to branch
    };

} // namespace SparkEditor