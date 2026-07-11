/**
 * @file SceneImportPanel.h
 * @brief One-way import of game INI .scene files into the editor's live ECS World (W10)
 * @author Spark Engine Team
 * @date 2026
 *
 * Reads the INI-dialect scene files the game runtime consumes
 * (Assets/Scenes/**.scene — [Object] sections with type/model/position/
 * rotation/scale/material keys, the same dialect SceneManager::LoadCustom and
 * TFWorldCollision::ParseScene parse) and instantiates each importable object
 * as a normal editor entity (NameComponent + Transform + MeshRenderer) in the
 * CURRENT document World. The whole import is ONE undoable command.
 *
 * Honesty contract (one-way flow):
 * - Imported entities are ordinary, fully editable entities — but File > Save
 *   Scene writes the EDITOR's reflected JSON scene format. Nothing is EVER
 *   written back to the source .scene file. The panel labels the flow
 *   "Import (one-way)" and shows the source path + imported entity count.
 * - Only [Object] nodes of type cube/model become entities. Everything else
 *   (spawnpoint, terrain, unknown types) is skipped and reported.
 * - type=cube maps to a placeholder mesh path: WorldMeshCache falls back to
 *   Mesh::CreateCube(1.0f), the same centered unit cube the game instantiates
 *   for cube primitives, so scaled cubes look identical to the runtime.
 * - Rotations copy through unchanged: .scene rotations are authored in
 *   degrees and the editor's ::Transform stores Euler degrees.
 *
 * Editor-only and VISUAL-ONLY: nothing here touches the server/client sim or
 * the determinism contract — the game keeps parsing its own .scene files.
 *
 * The parser is a small standalone re-implementation (no game-module code is
 * linked; module DLLs statically link the engine, so linking game parsers
 * into the editor is not an option).
 *
 * Accessible via Window > Scene Import.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <string>
#include <vector>

namespace SparkEditor
{

    class EditorUI;

    /**
     * @brief Editor panel that imports game INI .scene files into the current World.
     */
    class SceneImportPanel : public EditorPanel
    {
      public:
        SceneImportPanel();
        ~SceneImportPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "SceneImportPanel"; }

        /// @brief Non-owning access to EditorUI (current World + notifications).
        ///        Wired at registration in EditorPanelFactory (WorkflowPanel pattern);
        ///        the panel reads GetWorld() live each use, so no per-swap rewire is
        ///        needed (same as InspectorPanel).
        void SetEditorUI(EditorUI* ui) { m_editorUI = ui; }

      private:
        /// @brief One importable [Object] parsed out of a .scene file.
        struct SceneObjectRecord
        {
            std::string type;                          ///< "cube" or "model" (lower/upper accepted)
            std::string name;                          ///< name= key, or "<type>_<index>" fallback
            std::string model;                         ///< model= path; empty for cubes
            std::string material;                      ///< material= path (may be empty)
            float position[3] = {0.0f, 0.0f, 0.0f};    ///< world-space meters
            float rotationDeg[3] = {0.0f, 0.0f, 0.0f}; ///< Euler degrees (editor Transform convention)
            float scale[3] = {1.0f, 1.0f, 1.0f};       ///< non-uniform scale
        };

        /// @brief Full parse result for one .scene file.
        struct ParsedScene
        {
            std::string sceneName;                     ///< [Scene] name= (may be empty)
            std::string diskPath;                      ///< filesystem path that was parsed
            std::vector<SceneObjectRecord> objects;    ///< importable objects, file order
            std::vector<std::string> skippedTypes;     ///< one entry per skipped node ("spawnpoint", ...)
            std::vector<std::string> unresolvedModels; ///< unique model= paths not found on disk
        };

        /// @brief One .scene file discovered under Assets/Scenes.
        struct SceneFileEntry
        {
            std::string diskPath;    ///< openable from the editor's cwd
            std::string displayPath; ///< "MMOFPS/sanctuary_haven.scene" (relative to Assets/Scenes)
        };

        /// @brief Result of the last import, for the summary dialog + status line.
        struct ImportSummary
        {
            std::string sourcePath;                    ///< display path of the imported file
            int imported = 0;                          ///< entities created
            int skippedTotal = 0;                      ///< nodes skipped (spawnpoints, terrain, unknown)
            std::vector<std::string> skippedCounts;    ///< "spawnpoint x19", "terrain x1", ...
            std::vector<std::string> unresolvedModels; ///< model paths that render as placeholder cubes
        };

        void ScanSceneFiles();
        bool ParseSceneFile(const std::string& diskPath, ParsedScene& out) const;

        /// @brief Execute the import of @p parsed into the current World as ONE
        ///        undoable CommandHistory command (create-with-hint on redo, so
        ///        entity ids stay stable across undo/redo cycles).
        void ImportParsed(const ParsedScene& parsed, const std::string& displayPath);

        void RenderFileList();
        void RenderPreviewAndImport();
        void RenderSummaryPopup();

        /// @brief Aggregate a flat skipped-type list into "type xN" strings.
        static std::vector<std::string> AggregateSkipped(const std::vector<std::string>& skippedTypes);

        EditorUI* m_editorUI = nullptr; ///< Non-owning; owned by EditorApplication

        std::vector<SceneFileEntry> m_sceneFiles;
        int m_selected = -1;

        ParsedScene m_preview;       ///< parse of the currently selected file
        bool m_previewValid = false; ///< m_preview corresponds to m_selected

        ImportSummary m_lastImport;      ///< most recent import result
        bool m_hasImported = false;      ///< m_lastImport is populated
        bool m_openSummaryPopup = false; ///< request to open the summary modal

        /// Path prefix such that (m_assetsPrefix + "Assets/Scenes") exists ("", "../", ...).
        std::string m_assetsPrefix;
    };

} // namespace SparkEditor
