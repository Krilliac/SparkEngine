/**
 * @file VersionControlTypes.h
 * @brief Type definitions, structs, and base classes for the version control system
 * @author Spark Engine Team
 * @date 2025
 *
 * This file contains all data types used by VersionControlSystem, including
 * branch/commit/file-change structs, operation types, collaboration settings,
 * and asset merge handler base classes.
 */

#pragma once

#include <vector>
#include <memory>
#include <string>
#include <functional>
#include <chrono>
#include "../Enums/VersionControlEnums.h"

namespace SparkEditor
{

    /**
 * @brief Branch information
 */
    struct BranchInfo
    {
        std::string name;                                     ///< Branch name
        std::string commitHash;                               ///< Latest commit hash
        std::string author;                                   ///< Last commit author
        std::chrono::system_clock::time_point lastCommitTime; ///< Last commit time
        std::string description;                              ///< Branch description
        bool isRemote = false;                                ///< Whether branch is remote
        bool isCurrent = false;                               ///< Whether this is current branch
        bool isProtected = false;                             ///< Whether branch is protected
        int commitsAhead = 0;                                 ///< Commits ahead of upstream
        int commitsBehind = 0;                                ///< Commits behind upstream
    };

    /**
 * @brief Commit information
 */
    struct CommitInfo
    {
        std::string hash;                                ///< Commit hash
        std::string shortHash;                           ///< Short commit hash
        std::string message;                             ///< Commit message
        std::string author;                              ///< Author name
        std::string authorEmail;                         ///< Author email
        std::chrono::system_clock::time_point timestamp; ///< Commit timestamp
        std::vector<std::string> changedFiles;           ///< Files changed in commit
        std::vector<std::string> addedFiles;             ///< Files added in commit
        std::vector<std::string> deletedFiles;           ///< Files deleted in commit
        std::vector<std::string> renamedFiles;           ///< Files renamed in commit
        bool isMergeCommit = false;                      ///< Whether this is a merge commit
        std::vector<std::string> parentHashes;           ///< Parent commit hashes
        std::vector<std::string> tags;                   ///< Tags on this commit
    };

    /**
 * @brief File change information
 */
    struct FileChange
    {
        std::string filePath;                           ///< File path
        FileStatus status;                              ///< File status
        std::string conflictType;                       ///< Type of conflict (if any)
        size_t additions = 0;                           ///< Lines added
        size_t deletions = 0;                           ///< Lines deleted
        bool isBinary = false;                          ///< Whether file is binary
        bool isLFS = false;                             ///< Whether file uses LFS
        std::string lockedBy;                           ///< User who locked file (if locked)
        std::chrono::system_clock::time_point lockTime; ///< When file was locked

        // Merge conflict data
        std::string baseVersion;         ///< Base version content
        std::string localVersion;        ///< Local version content
        std::string remoteVersion;       ///< Remote version content
        std::string mergedVersion;       ///< Merged version content
        bool hasConflictMarkers = false; ///< Whether file has conflict markers
    };

    /**
 * @brief Merge conflict information
 */
    struct MergeConflict
    {
        std::string filePath;                      ///< Conflicted file path
        std::string conflictType;                  ///< Type of conflict
        std::string description;                   ///< Conflict description
        std::vector<std::string> conflictSections; ///< Conflicted sections
        bool isResolved = false;                   ///< Whether conflict is resolved
        std::string resolution;                    ///< Resolution method used

        enum ConflictType
        {
            CONTENT_CONFLICT = 0,
            RENAME_CONFLICT = 1,
            DELETE_CONFLICT = 2,
            BINARY_CONFLICT = 3,
            ASSET_CONFLICT = 4,
            METADATA_CONFLICT = 5
        };
    };

    /**
 * @brief Repository information
 */
    struct RepositoryInfo
    {
        std::string path;                     ///< Repository root path
        VCSType type;                         ///< Version control system type
        std::string remoteURL;                ///< Remote repository URL
        std::string remoteName = "origin";    ///< Remote name
        BranchInfo currentBranch;             ///< Current branch
        std::vector<BranchInfo> branches;     ///< All branches
        std::vector<FileChange> changedFiles; ///< Currently changed files
        std::vector<MergeConflict> conflicts; ///< Current merge conflicts
        bool hasUncommittedChanges = false;   ///< Whether there are uncommitted changes
        bool isClean = true;                  ///< Whether working directory is clean
        bool hasLFS = false;                  ///< Whether repository uses LFS
        std::string lfsVersion;               ///< LFS version
    };

    /**
 * @brief User information for commits
 */
    struct UserInfo
    {
        std::string name;                 ///< User name
        std::string email;                ///< User email
        std::string avatarPath;           ///< Path to user avatar image
        std::vector<std::string> sshKeys; ///< SSH key paths
        std::string gpgKey;               ///< GPG key for signing
        bool signCommits = false;         ///< Whether to sign commits
    };

    /**
 * @brief VCS operation result
 */
    struct VCSOperationResult
    {
        bool success = false;              ///< Whether operation succeeded
        std::string errorMessage;          ///< Error message if failed
        std::string output;                ///< Command output
        int exitCode = 0;                  ///< Exit code
        float duration = 0.0f;             ///< Operation duration in seconds
        std::vector<std::string> warnings; ///< Warning messages
    };

    /**
 * @brief Async VCS operation
 */
    struct VCSOperation
    {
        enum Type
        {
            CLONE = 0,
            PULL = 1,
            PUSH = 2,
            COMMIT = 3,
            MERGE = 4,
            CHECKOUT = 5,
            FETCH = 6,
            STATUS = 7,
            LOG = 8,
            DIFF = 9,
            CUSTOM = 10
        } type;

        std::string description;                                 ///< Operation description
        std::function<VCSOperationResult()> function;            ///< Operation function
        std::function<void(const VCSOperationResult&)> callback; ///< Completion callback
        std::function<void(float)> progressCallback;             ///< Progress callback
        int priority = 0;                                        ///< Operation priority
        std::chrono::steady_clock::time_point submitTime;        ///< Submit time
        bool isRunning = false;                                  ///< Whether operation is running
        float progress = 0.0f;                                   ///< Operation progress (0-1)
    };

    /**
 * @brief Collaboration settings
 */
    struct CollaborationSettings
    {
        bool enableRealtimeSync = false;      ///< Enable real-time synchronization
        bool enableFileLocking = true;        ///< Enable file locking
        bool enableAutoMerge = true;          ///< Enable automatic merging
        bool enableConflictResolution = true; ///< Enable conflict resolution UI
        bool enableActivityFeed = true;       ///< Enable activity feed
        bool enablePresenceIndicators = true; ///< Enable user presence indicators

        // Auto-sync settings
        float autoSyncInterval = 60.0f; ///< Auto-sync interval (seconds)
        bool autoSyncOnSave = true;     ///< Auto-sync when saving files
        bool autoSyncOnIdle = true;     ///< Auto-sync when idle
        float idleTimeout = 300.0f;     ///< Idle timeout (seconds)

        // Notification settings
        bool notifyOnConflicts = true;        ///< Notify on merge conflicts
        bool notifyOnUpdates = true;          ///< Notify on remote updates
        bool notifyOnLocks = true;            ///< Notify on file locks
        bool showDesktopNotifications = true; ///< Show desktop notifications

        // Merge strategy
        enum MergeStrategy
        {
            MANUAL = 0,
            AUTO_MERGE = 1,
            PREFER_LOCAL = 2,
            PREFER_REMOTE = 3,
            SMART_MERGE = 4
        } mergeStrategy = SMART_MERGE;
    };

    /**
 * @brief Asset merge handler
 */
    class AssetMergeHandler
    {
      public:
        /**
     * @brief Virtual destructor
     */
        virtual ~AssetMergeHandler() = default;

        /**
     * @brief Get supported file extensions
     * @return Vector of supported extensions
     */
        virtual std::vector<std::string> GetSupportedExtensions() const = 0;

        /**
     * @brief Check if handler can merge file
     * @param filePath File path to check
     * @return true if handler can merge the file
     */
        virtual bool CanMerge(const std::string& filePath) const = 0;

        /**
     * @brief Perform automatic merge
     * @param conflict Merge conflict to resolve
     * @return true if merge was successful
     */
        virtual bool AutoMerge(MergeConflict& conflict) = 0;

        /**
     * @brief Show merge UI for manual resolution
     * @param conflict Merge conflict to resolve
     * @return true if conflict was resolved
     */
        virtual bool ShowMergeUI(MergeConflict& conflict) = 0;

        /**
     * @brief Validate merged result
     * @param filePath Path to merged file
     * @return true if merged file is valid
     */
        virtual bool ValidateMerge(const std::string& filePath) = 0;
    };

    /**
 * @brief Scene merge handler
 */
    class SceneMergeHandler : public AssetMergeHandler
    {
      public:
        std::vector<std::string> GetSupportedExtensions() const override;
        bool CanMerge(const std::string& filePath) const override;
        bool AutoMerge(MergeConflict& conflict) override;
        bool ShowMergeUI(MergeConflict& conflict) override;
        bool ValidateMerge(const std::string& filePath) override;
    };

    /**
 * @brief Material merge handler
 */
    class MaterialMergeHandler : public AssetMergeHandler
    {
      public:
        std::vector<std::string> GetSupportedExtensions() const override;
        bool CanMerge(const std::string& filePath) const override;
        bool AutoMerge(MergeConflict& conflict) override;
        bool ShowMergeUI(MergeConflict& conflict) override;
        bool ValidateMerge(const std::string& filePath) override;
    };

} // namespace SparkEditor
