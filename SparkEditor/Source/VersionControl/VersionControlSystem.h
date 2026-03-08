/**
 * @file VersionControlSystem.h
 * @brief Version control integration system for collaborative development in Spark Engine
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file implements comprehensive version control integration with Git,
 * including LFS support, asset merging, conflict resolution, and collaborative
 * editing features similar to Unity Collaborate and Perforce integration.
 */

#pragma once

#include "../Core/EditorPanel.h"
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <functional>
#include <thread>
#include <mutex>
#include <atomic>
#include <queue>
#include <chrono>
#include <filesystem>
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

    /**
 * @brief Version control integration system
 * 
 * Provides comprehensive version control capabilities including:
 * - Git integration with full feature support
 * - Large File Storage (LFS) support for binary assets
 * - Intelligent asset merging and conflict resolution
 * - Real-time collaborative editing features
 * - File locking and user presence indicators
 * - Automated synchronization and backup
 * - Branch management and workflow integration
 * - Activity feeds and change notifications
 * - Integration with asset pipeline and build system
 * 
 * Inspired by Unity Collaborate, Perforce integration, and GitHub Desktop.
 */
    class VersionControlSystem : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        VersionControlSystem();

        /**
     * @brief Destructor
     */
        ~VersionControlSystem() override;

        /**
     * @brief Initialize the version control system
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update version control system
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render version control UI
     */
        void Render() override;

        /**
     * @brief Shutdown the version control system
     */
        void Shutdown() override;

        /**
     * @brief Handle panel events
     * @param eventType Event type
     * @param eventData Event data
     * @return true if event was handled
     */
        bool HandleEvent(const std::string& eventType, void* eventData) override;

        /**
     * @brief Initialize repository in directory
     * @param directoryPath Directory path
     * @param vcsType Version control system type
     * @return Operation result
     */
        VCSOperationResult InitializeRepository(const std::string& directoryPath, VCSType vcsType = VCSType::GIT);

        /**
     * @brief Clone repository from URL
     * @param repositoryURL Repository URL
     * @param localPath Local path to clone to
     * @param progressCallback Progress callback
     * @return Operation result
     */
        VCSOperationResult CloneRepository(const std::string& repositoryURL, const std::string& localPath,
                                           std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Open existing repository
     * @param repositoryPath Path to repository
     * @return true if repository was opened successfully
     */
        bool OpenRepository(const std::string& repositoryPath);

        /**
     * @brief Close current repository
     */
        void CloseRepository();

        /**
     * @brief Get current repository information
     * @return Pointer to repository info, or nullptr if no repository
     */
        const RepositoryInfo* GetRepositoryInfo() const;

        /**
     * @brief Refresh repository status
     * @param callback Completion callback
     */
        void RefreshStatus(std::function<void()> callback = nullptr);

        /**
     * @brief Stage files for commit
     * @param filePaths Files to stage
     * @return Operation result
     */
        VCSOperationResult StageFiles(const std::vector<std::string>& filePaths);

        /**
     * @brief Unstage files
     * @param filePaths Files to unstage
     * @return Operation result
     */
        VCSOperationResult UnstageFiles(const std::vector<std::string>& filePaths);

        /**
     * @brief Commit staged changes
     * @param message Commit message
     * @param description Commit description (optional)
     * @return Operation result
     */
        VCSOperationResult Commit(const std::string& message, const std::string& description = "");

        /**
     * @brief Push changes to remote
     * @param remoteName Remote name (default: origin)
     * @param branchName Branch name (default: current)
     * @param progressCallback Progress callback
     * @return Operation result
     */
        VCSOperationResult Push(const std::string& remoteName = "origin", const std::string& branchName = "",
                                std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Pull changes from remote
     * @param remoteName Remote name (default: origin)
     * @param branchName Branch name (default: current)
     * @param progressCallback Progress callback
     * @return Operation result
     */
        VCSOperationResult Pull(const std::string& remoteName = "origin", const std::string& branchName = "",
                                std::function<void(float)> progressCallback = nullptr);

        /**
     * @brief Fetch changes from remote
     * @param remoteName Remote name (default: origin)
     * @return Operation result
     */
        VCSOperationResult Fetch(const std::string& remoteName = "origin");

        /**
     * @brief Create new branch
     * @param branchName Branch name
     * @param baseBranch Base branch (default: current)
     * @return Operation result
     */
        VCSOperationResult CreateBranch(const std::string& branchName, const std::string& baseBranch = "");

        /**
     * @brief Switch to branch
     * @param branchName Branch name
     * @return Operation result
     */
        VCSOperationResult SwitchBranch(const std::string& branchName);

        /**
     * @brief Merge branch into current branch
     * @param branchName Branch to merge
     * @return Operation result
     */
        VCSOperationResult MergeBranch(const std::string& branchName);

        /**
     * @brief Delete branch
     * @param branchName Branch name
     * @param force Force deletion
     * @return Operation result
     */
        VCSOperationResult DeleteBranch(const std::string& branchName, bool force = false);

        /**
     * @brief Get commit history
     * @param maxCommits Maximum number of commits
     * @param branchName Branch name (default: current)
     * @return Vector of commit information
     */
        std::vector<CommitInfo> GetCommitHistory(int maxCommits = 100, const std::string& branchName = "");

        /**
     * @brief Get file differences
     * @param filePath File path
     * @param commitHash1 First commit hash (default: working directory)
     * @param commitHash2 Second commit hash (default: HEAD)
     * @return File differences as string
     */
        std::string GetFileDiff(const std::string& filePath, const std::string& commitHash1 = "",
                                const std::string& commitHash2 = "");

        /**
     * @brief Revert file to last committed version
     * @param filePath File path
     * @return Operation result
     */
        VCSOperationResult RevertFile(const std::string& filePath);

        /**
     * @brief Lock file for exclusive editing
     * @param filePath File path
     * @return Operation result
     */
        VCSOperationResult LockFile(const std::string& filePath);

        /**
     * @brief Unlock file
     * @param filePath File path
     * @return Operation result
     */
        VCSOperationResult UnlockFile(const std::string& filePath);

        /**
     * @brief Resolve merge conflict
     * @param conflict Conflict to resolve
     * @param resolution Resolution method
     * @return true if conflict was resolved
     */
        bool ResolveMergeConflict(MergeConflict& conflict, const std::string& resolution);

        /**
     * @brief Register asset merge handler
     * @param handler Merge handler to register
     */
        void RegisterMergeHandler(std::unique_ptr<AssetMergeHandler> handler);

        /**
     * @brief Set user information
     * @param userInfo User information
     */
        void SetUserInfo(const UserInfo& userInfo);

        /**
     * @brief Get user information
     * @return Reference to user information
     */
        const UserInfo& GetUserInfo() const { return m_userInfo; }

        /**
     * @brief Set collaboration settings
     * @param settings Collaboration settings
     */
        void SetCollaborationSettings(const CollaborationSettings& settings);

        /**
     * @brief Get collaboration settings
     * @return Reference to collaboration settings
     */
        const CollaborationSettings& GetCollaborationSettings() const { return m_collaborationSettings; }

        /**
     * @brief Enable/disable version control
     * @param enabled Whether version control is enabled
     */
        void SetEnabled(bool enabled) { m_isEnabled = enabled; }

        /**
     * @brief Check if version control is enabled
     * @return true if version control is enabled
     */
        bool IsEnabled() const { return m_isEnabled; }

        /**
     * @brief Get file status
     * @param filePath File path
     * @return File status
     */
        FileStatus GetFileStatus(const std::string& filePath) const;

        /**
     * @brief Check if file is tracked
     * @param filePath File path
     * @return true if file is tracked
     */
        bool IsFileTracked(const std::string& filePath) const;

        /**
     * @brief Check if file is locked
     * @param filePath File path
     * @return true if file is locked
     */
        bool IsFileLocked(const std::string& filePath) const;

        /**
     * @brief Get active users in repository
     * @return Vector of active user names
     */
        std::vector<std::string> GetActiveUsers() const;

        /**
     * @brief Add ignore pattern
     * @param pattern Pattern to ignore
     * @return true if pattern was added
     */
        bool AddIgnorePattern(const std::string& pattern);

        /**
     * @brief Remove ignore pattern
     * @param pattern Pattern to remove
     * @return true if pattern was removed
     */
        bool RemoveIgnorePattern(const std::string& pattern);

        /**
     * @brief Get ignore patterns
     * @return Vector of ignore patterns
     */
        std::vector<std::string> GetIgnorePatterns() const;

      private:
        /**
     * @brief Render repository overview
     */
        void RenderRepositoryOverview();

        /**
     * @brief Render changes panel
     */
        void RenderChangesPanel();

        /**
     * @brief Render history panel
     */
        void RenderHistoryPanel();

        /**
     * @brief Render branches panel
     */
        void RenderBranchesPanel();

        /**
     * @brief Render conflicts panel
     */
        void RenderConflictsPanel();

        /**
     * @brief Render settings panel
     */
        void RenderSettingsPanel();

        /**
     * @brief Process VCS operations queue
     */
        void ProcessOperationQueue();

        /**
     * @brief Execute VCS command
     * @param command Command to execute
     * @param workingDirectory Working directory
     * @return Operation result
     */
        VCSOperationResult ExecuteCommand(const std::string& command, const std::string& workingDirectory = "");

        /**
     * @brief Parse Git status output
     * @param output Git status output
     * @return Vector of file changes
     */
        std::vector<FileChange> ParseGitStatus(const std::string& output);

        /**
     * @brief Parse Git log output
     * @param output Git log output
     * @return Vector of commit information
     */
        std::vector<CommitInfo> ParseGitLog(const std::string& output);

        /**
     * @brief Parse Git branch output
     * @param output Git branch output
     * @return Vector of branch information
     */
        std::vector<BranchInfo> ParseGitBranches(const std::string& output);

        /**
     * @brief Update file system watcher
     */
        void UpdateFileSystemWatcher();

        /**
     * @brief Handle file system changes
     * @param changedFiles Files that changed
     */
        void HandleFileSystemChanges(const std::vector<std::string>& changedFiles);

        /**
     * @brief Auto-sync if enabled
     */
        void AutoSync();

        /**
     * @brief Detect merge conflicts
     */
        void DetectMergeConflicts();

        /**
     * @brief Auto-resolve conflicts if possible
     */
        void AutoResolveConflicts();

        /**
     * @brief Get appropriate merge handler for file
     * @param filePath File path
     * @return Pointer to merge handler, or nullptr if none found
     */
        AssetMergeHandler* GetMergeHandler(const std::string& filePath);

        /**
     * @brief Initialize LFS if needed
     * @return true if LFS is ready
     */
        bool InitializeLFS();

        /**
     * @brief Check if file should use LFS
     * @param filePath File path
     * @return true if file should use LFS
     */
        bool ShouldUseLFS(const std::string& filePath) const;

      private:
        // Repository state
        std::unique_ptr<RepositoryInfo> m_repositoryInfo; ///< Current repository information
        bool m_isEnabled = false;                         ///< Whether version control is enabled
        VCSType m_vcsType = VCSType::GIT;                 ///< Version control system type

        // User information
        UserInfo m_userInfo;                           ///< User information
        CollaborationSettings m_collaborationSettings; ///< Collaboration settings

        // Operations queue
        std::queue<VCSOperation> m_operationQueue;       ///< Pending operations queue
        std::thread m_operationThread;                   ///< Operation processing thread
        std::mutex m_operationMutex;                     ///< Operation queue mutex
        std::condition_variable m_operationCondition;    ///< Operation queue condition
        std::atomic<bool> m_shouldStopOperations{false}; ///< Stop operations flag

        // Merge handlers
        std::vector<std::unique_ptr<AssetMergeHandler>> m_mergeHandlers; ///< Asset merge handlers

        // File system watching
        std::thread m_fileWatcherThread;                      ///< File system watcher thread
        std::atomic<bool> m_shouldStopWatcher{false};         ///< Stop watcher flag
        std::chrono::steady_clock::time_point m_lastAutoSync; ///< Last auto-sync time

        // UI state
        bool m_showRepository = true; ///< Show repository panel
        bool m_showChanges = true;    ///< Show changes panel
        bool m_showHistory = true;    ///< Show history panel
        bool m_showBranches = false;  ///< Show branches panel
        bool m_showConflicts = false; ///< Show conflicts panel
        bool m_showSettings = false;  ///< Show settings panel

        // Commit UI
        std::string m_commitMessage;            ///< Current commit message
        std::string m_commitDescription;        ///< Current commit description
        std::vector<std::string> m_stagedFiles; ///< Currently staged files

        // History UI
        std::vector<CommitInfo> m_commitHistory; ///< Commit history
        std::string m_selectedCommit;            ///< Selected commit hash

        // Status cache
        std::unordered_map<std::string, FileStatus> m_fileStatusCache; ///< File status cache
        std::chrono::steady_clock::time_point m_lastStatusUpdate;      ///< Last status update time
        mutable std::mutex m_statusMutex;                              ///< Status cache mutex

        // LFS configuration
        std::vector<std::string> m_lfsPatterns;    ///< LFS file patterns
        size_t m_lfsThreshold = 100 * 1024 * 1024; ///< LFS size threshold (100MB)

        // Performance settings
        float m_statusUpdateInterval = 5.0f;      ///< Status update interval (seconds)
        int m_maxHistoryEntries = 1000;           ///< Maximum history entries to load
        bool m_enableBackgroundOperations = true; ///< Enable background operations
    };

} // namespace SparkEditor