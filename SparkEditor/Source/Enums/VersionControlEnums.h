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
 */
    enum class BranchOperation
    {
        CREATE = 0, ///< Create new branch
        DELETE = 1, ///< Delete branch
        RENAME = 2, ///< Rename branch
        MERGE = 3,  ///< Merge branch
        REBASE = 4, ///< Rebase branch
        SWITCH = 5  ///< Switch to branch
    };

} // namespace SparkEditor