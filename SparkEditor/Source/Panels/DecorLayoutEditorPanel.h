/**
 * @file DecorLayoutEditorPanel.h
 * @brief Visual editor for the TERRAFRONT region decor templates (Assets/MMOFPS/Data/decor.json) — W12
 * @author Spark Engine Team
 * @date 2026
 *
 * A 2D top-down canvas (ImGui drawlist) per tier template (outpost/fort/
 * facility/skyanchor): pieces are drawn as labeled footprint rectangles whose
 * bounds come from streaming the OBJ vertex min/max (a local reader mirroring
 * TFWorldCollision::ObjLocalAabb — the editor never links module code).
 * Pieces drag by offset and rotate in 15-degree steps; the collide flag is
 * color-coded; W11 "collideParts" boxes render nested inside their piece and
 * are add/remove/drag/resizable. A clearance overlay shows the standard
 * capture-point clearance circle (clearanceM, 8 m) plus a 0.8 m pawn-capsule
 * circle at a draggable probe so authors see walkability at a glance.
 *
 * This panel edits the DATA FILE only — it never touches the live world.
 * The emitted schema mirrors exactly what TFRegionDecor::LoadTemplates
 * consumes (see the .cpp header comment for the field-by-field mapping).
 * Save follows the RegionMapEditorPanel house pattern: .bak backup first,
 * stable-order hand-rolled writer, ParseStrict re-read of the written bytes,
 * backup restore on failure.
 */

#pragma once

#include "../Core/EditorPanel.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace SparkEditor
{

    /// @brief Visual editor for Assets/MMOFPS/Data/decor.json (DATA only, no live world).
    class DecorLayoutEditorPanel : public EditorPanel
    {
      public:
        // ============================================================
        // Editable mirror of the schema consumed by TFRegionDecor::LoadTemplates
        // ============================================================

        /// One authored collision box of a multi-part piece (W11 gate-passages).
        /// MODEL-LOCAL meters: `off` is the box center relative to the piece
        /// origin, `size` the FULL box size, `yawDeg` a local yaw composed with
        /// the piece yaw. Authoring parts implies collide in the game loader.
        struct CollidePart
        {
            std::array<float, 3> off = {0.0f, 0.0f, 0.0f};
            std::array<float, 3> size = {1.0f, 1.0f, 1.0f};
            float yawDeg = 0.0f;
        };

        struct Piece
        {
            std::string model;    ///< OBJ path (Assets/Models/MMOFPS/...)
            std::string material; ///< empty = faction/neutral structure material
            float offX = 0.0f;    ///< meters east of region center
            float offZ = 0.0f;    ///< meters north of region center
            float yawDeg = 0.0f;  ///< Transform.rotation is DEGREES
            bool terrainAlign = true;
            bool castShadows = true;
            float emissive = 0.0f;
            bool collide = false; ///< whole-model OBB (ignored when parts exist)
            std::vector<CollidePart> collideParts;
        };

        struct Scatter
        {
            std::vector<std::string> models;
            int countMin = 0;
            int countMax = 0;
            float radiusMin = 0.0f;
            float radiusMax = 0.0f;
        };

        struct TierTemplate
        {
            bool present = false; ///< tier key existed in the file / was created
            std::vector<Piece> pieces;
            Scatter scatter;
        };

        // ============================================================
        // Construction / EditorPanel interface
        // ============================================================

        DecorLayoutEditorPanel();
        ~DecorLayoutEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

        std::string GetTypeName() const override { return "DecorLayoutEditorPanel"; }
        bool IsModified() const override { return m_dirty; }

        /// @brief Absolute-ish path (after prefix probing) of the file being edited.
        const std::string& GetDataPath() const { return m_dataPath; }

      private:
        /// Cached OBJ vertex bounds (local reader, same streaming contract as
        /// TFWorldCollision::ObjLocalAabb). !valid => unreadable/missing file;
        /// the canvas then draws the unit-cube fallback footprint dashed.
        struct ObjBounds
        {
            bool valid = false;
            std::array<float, 3> mn = {-0.5f, -0.5f, -0.5f};
            std::array<float, 3> mx = {0.5f, 0.5f, 0.5f};
        };

        // -- Load / save --------------------------------------------------------
        void ResolveDataPath();
        bool LoadFromDisk(std::string& outError);
        bool SaveToDisk(std::string& outError);
        std::string SerializeDocument() const;

        // -- OBJ footprints -------------------------------------------------------
        const ObjBounds& BoundsForModel(const std::string& model);

        // -- Validation ---------------------------------------------------------
        void RunValidation();

        // -- Rendering ----------------------------------------------------------
        void RenderToolbar();
        void RenderCanvas(TierTemplate& t);
        void RenderSidePane(TierTemplate& t);
        void RenderPieceEditor(TierTemplate& t);
        void RenderTierEditor(TierTemplate& t);
        void RenderValidationAndSave();

        // -- Editing helpers ----------------------------------------------------
        void SelectPiece(int piece, int part);
        void SyncEditBuffers();
        void MarkDirty();

        // -- Document state -----------------------------------------------------
        std::string m_assetsPrefix;          ///< "", "../", ... — probed at Initialize
        std::string m_dataPath;              ///< <prefix>Assets/MMOFPS/Data/decor.json
        std::string m_schemaNote;            ///< preserved "$schema_note" (may be empty)
        float m_clearanceM = 8.0f;           ///< root "clearanceM"
        std::array<TierTemplate, 4> m_tiers; ///< kTierNames order (outpost/fort/facility/skyanchor)
        bool m_loaded = false;
        bool m_dirty = false;
        std::string m_statusMsg; ///< last load/save result (shown in side pane)
        bool m_statusIsError = false;

        std::unordered_map<std::string, ObjBounds> m_boundsCache; ///< by model path (misses cached too)

        // -- Selection / interaction state ---------------------------------------
        int m_tier = 0; ///< active tier tab, index into m_tiers
        int m_selPiece = -1;
        int m_selPart = -1; ///< index into selected piece's collideParts

        enum class DragKind : uint8_t
        {
            None,
            PieceMove,
            PieceRotate,
            PartMove,
            PartResize,
            Probe
        };
        DragKind m_dragKind = DragKind::None;

        // -- Clearance preview ------------------------------------------------------
        bool m_showClearance = true;
        float m_probeX = 0.0f; ///< draggable probe (meters from region center)
        float m_probeZ = 0.0f;

        // -- View transform -------------------------------------------------------
        float m_zoom = 1.0f;
        float m_panX = 0.0f;
        float m_panY = 0.0f;
        float m_viewExtent = 80.0f; ///< world half-extent mapped by Fit View
        bool m_fitRequested = true;

        // -- Save gating -----------------------------------------------------------
        std::vector<std::string> m_violations;
        bool m_validationRan = false;
        bool m_overrideSave = false; ///< 'I know what I am doing' checkbox

        // -- Side-pane edit buffers (synced on selection change) -------------------
        int m_bufTier = -1;
        int m_bufPiece = -1;
        char m_modelBuf[256] = {};
        char m_materialBuf[256] = {};
        char m_newModelBuf[256] = {};
        char m_newScatterBuf[256] = {};
    };

} // namespace SparkEditor
