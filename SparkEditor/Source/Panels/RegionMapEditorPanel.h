/**
 * @file RegionMapEditorPanel.h
 * @brief Visual editor for the TERRAFRONT territory lattice (Assets/MMOFPS/Data/regions.json) — W11
 * @author Spark Engine Team
 * @date 2026
 *
 * A 2D map canvas (ImGui drawlist) that shows regions as tier-colored hexes at
 * their world centers, conduit links as lines, and capture points / spawns /
 * vehicle terminals as draggable markers. A side pane edits scalar fields, and
 * a validated writer saves the file back with a stable field order.
 *
 * This panel edits the DATA FILE only — it never touches the live world.
 * The emitted schema mirrors exactly what TFDataTables::ParseRegions consumes
 * (see the .cpp header comment for the field-by-field mapping).
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace SparkEditor
{

    /// @brief Visual editor for Assets/MMOFPS/Data/regions.json (DATA only, no live world).
    class RegionMapEditorPanel : public EditorPanel
    {
      public:
        // ============================================================
        // Editable mirror of the schema consumed by TFDataTables::ParseRegions
        // ============================================================

        struct Region
        {
            int id = 0;
            std::string key;
            std::string name;
            std::string tier;        ///< skyanchor|outpost|fort|facility
            std::string homeFaction; ///< "", "MRA", "AUC", "HLX" (empty = absent from file)
            bool hasHex = false;
            int hexQ = 0;
            int hexR = 0;
            float centerX = 0.0f;
            float centerZ = 0.0f;
            float captureSec = 60.0f;
            int fluxPerTick = 0;
            std::vector<std::array<float, 2>> capturePoints;
            std::vector<std::array<float, 2>> spawns;
            bool hasVehicleTerminal = false;
            std::array<float, 2> vehicleTerminal = {0.0f, 0.0f};
            std::string owner; ///< initialOwnership bucket: "MRA"|"AUC"|"HLX"|"neutral"; "" = unassigned
        };

        struct Continent
        {
            std::string name = "Cindral Wastes";
            float sizeM = 4096.0f;
            std::string scene;
            float fluxTickSec = 60.0f;
        };

        // ============================================================
        // Construction / EditorPanel interface
        // ============================================================

        RegionMapEditorPanel();
        ~RegionMapEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "RegionMapEditorPanel"; }
        bool IsModified() const override { return m_dirty; }

        /// @brief Absolute-ish path (after prefix probing) of the file being edited.
        const std::string& GetDataPath() const { return m_dataPath; }

      private:
        // -- Load / save --------------------------------------------------------
        void ResolveDataPath();
        bool LoadFromDisk(std::string& outError);
        bool SaveToDisk(std::string& outError);
        std::string SerializeDocument() const;

        // -- Validation ---------------------------------------------------------
        void RunValidation();
        float RegionRadiusHeuristic(size_t regionIndex) const;

        // -- Rendering ----------------------------------------------------------
        void RenderToolbar();
        void RenderCanvas();
        void RenderSidePane();
        void RenderRegionEditor();
        void RenderContinentEditor();
        void RenderValidationAndSave();

        // -- Editing helpers ----------------------------------------------------
        void ToggleConduit(int a, int b);
        void SelectRegion(int index);
        void SyncEditBuffers();

        // -- Document state -----------------------------------------------------
        std::string m_assetsPrefix; ///< "", "../", ... — probed at Initialize
        std::string m_dataPath;     ///< <prefix>Assets/MMOFPS/Data/regions.json
        std::string m_schemaNote;   ///< preserved "$schema_note" (may be empty)
        Continent m_continent;
        std::vector<Region> m_regions;               ///< kept sorted by id
        std::vector<std::pair<int, int>> m_conduits; ///< file order preserved
        bool m_loaded = false;
        bool m_dirty = false;
        std::string m_statusMsg; ///< last load/save result (shown in side pane)
        bool m_statusIsError = false;

        // -- Selection / interaction state ---------------------------------------
        int m_selected = -1; ///< index into m_regions
        bool m_linkMode = false;
        int m_linkFirst = -1; ///< first region clicked in link mode
        bool m_movePointsWithRegion = true;

        enum class DragKind : uint8_t
        {
            None,
            RegionCenter,
            CapturePoint,
            Spawn,
            Terminal
        };
        DragKind m_dragKind = DragKind::None;
        int m_dragRegion = -1;
        int m_dragPoint = -1;

        // -- View transform -------------------------------------------------------
        float m_zoom = 1.0f;
        float m_panX = 0.0f;
        float m_panY = 0.0f;
        bool m_fitRequested = true;

        // -- Save gating -----------------------------------------------------------
        std::vector<std::string> m_violations;
        bool m_validationRan = false;
        bool m_overrideSave = false; ///< 'I know what I am doing' checkbox

        // -- Side-pane edit buffers (synced on selection change) -------------------
        int m_bufForRegion = -1;
        char m_keyBuf[64] = {};
        char m_nameBuf[128] = {};
        char m_continentNameBuf[128] = {};
        char m_sceneBuf[256] = {};
    };

} // namespace SparkEditor
