/**
 * @file EditorRecovery.h
 * @brief Durable, UI-thread-owned recovery snapshots for SparkEditor.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class World;

namespace SparkEditor
{
    inline constexpr uint32_t kEditorRecoverySchemaVersion = 1;
    inline constexpr size_t kEditorRecoveryMaxBytes = 16u * 1024u * 1024u;

    struct EditorRecoverySnapshot
    {
        uint32_t schemaVersion = kEditorRecoverySchemaVersion;
        std::string projectIdentity;
        std::string projectRelativeScene;
        std::string sceneDisplayName;
        std::string serializedWorld;
        std::string layoutIniPath;
        std::vector<std::string> recentOperations;
        uint64_t dirtySequence = 0;
        int64_t capturedUnixMilliseconds = 0;
    };

    /**
     * @brief Serialize a recovery value while the caller owns the World.
     *
     * The helper deliberately takes a const World reference rather than storing
     * one: EditorUI calls it only from its owning thread before handing the
     * resulting value-only snapshot to EditorRecoveryStore.
     */
    EditorRecoverySnapshot CaptureRecoverySnapshotOnCallingThread(const ::World& world,
                                                                   EditorRecoverySnapshot metadata);

    /**
     * @brief Deserialize a recovery document into a new World without touching
     * the caller's live document.
     *
     * EditorUI owns the returned World and calls SwapWorld only after this
     * helper succeeds. The controller deliberately does not participate in
     * this operation so it remains state-only.
     */
    std::unique_ptr<::World> DeserializeRecoverySnapshotIntoFreshWorld(const EditorRecoverySnapshot& snapshot,
                                                                        std::string& error);

    /**
     * @brief Resolve a candidate scene path only when it remains inside a project.
     *
     * Relative candidates are interpreted beneath @p projectRoot. Both inputs
     * are weakly canonicalized so an existing symlink or junction cannot turn
     * a lexically safe-looking path into an outside-project write.
     */
    bool ResolvePathInsideProject(const std::filesystem::path& projectRoot,
                                  const std::filesystem::path& candidate,
                                  std::filesystem::path& resolved,
                                  std::string& error);

    enum class EditorRecoveryLoadState : uint8_t
    {
        None,
        Primary,
        Backup,
        Invalid
    };

    struct EditorRecoveryLoadResult
    {
        EditorRecoveryLoadState state = EditorRecoveryLoadState::None;
        std::optional<EditorRecoverySnapshot> snapshot;
        std::string error;
    };

    enum class EditorRecoveryDialogState : uint8_t
    {
        Hidden,
        Available,
        RestoreFailed
    };

    /**
     * @brief State-only recovery decision controller for the EditorUI modal.
     *
     * It intentionally owns neither a World nor a filesystem handle. Restore
     * and discard effects are performed by EditorUI only after the user's
     * explicit choice succeeds.
     */
    class EditorRecoveryController
    {
      public:
        void Offer(EditorRecoverySnapshot snapshot, bool usedBackup = false);
        void SetRestoreFailure(std::string error);
        void DismissAfterRestore();
        void DismissAfterDiscard();

        [[nodiscard]] EditorRecoveryDialogState State() const { return m_state; }
        [[nodiscard]] const EditorRecoverySnapshot* Snapshot() const;
        [[nodiscard]] std::string_view Error() const { return m_error; }
        [[nodiscard]] bool UsesBackup() const { return m_usesBackup; }

      private:
        EditorRecoveryDialogState m_state = EditorRecoveryDialogState::Hidden;
        std::optional<EditorRecoverySnapshot> m_snapshot;
        std::string m_error;
        bool m_usesBackup = false;
    };

    /**
     * @brief Keeps an explicit document discard from being re-captured during
     * the same editor session.
     *
     * The UI resets this policy after a World replacement. A mutation that
     * happens after a cancelled transition deliberately re-enables capture:
     * it is new user work, not the work the user previously discarded.
     */
    class EditorRecoveryCaptureGate
    {
      public:
        void SuppressAfterExplicitDiscard() { m_suppressed = true; }
        void NoteNewMutation() { m_suppressed = false; }
        void ResetForNewDocument() { m_suppressed = false; }

        [[nodiscard]] bool AllowsCapture() const { return !m_suppressed; }

      private:
        bool m_suppressed = false;
    };

    class EditorRecoveryStore
    {
      public:
        explicit EditorRecoveryStore(std::filesystem::path directory);

        bool Save(const EditorRecoverySnapshot& snapshot, std::string& error);
        bool Clear(std::string& error) const;
        /** @brief Remove only valid recovery records for the given project identity. */
        bool ClearForProject(std::string_view projectIdentity, std::string& error) const;
        EditorRecoveryLoadResult LoadForProject(std::string_view projectIdentity) const;

      private:
        std::filesystem::path m_directory;
    };
} // namespace SparkEditor
