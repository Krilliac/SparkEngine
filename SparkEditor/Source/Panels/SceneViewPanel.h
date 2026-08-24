/**
 * @file SceneViewPanel.h
 * @brief Scene view panel for 3D scene rendering in the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#include "../Communication/CollaborativeEditSession.h"
#include "Engine/ECS/Components.h"
#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#include "Graphics/WorldBasicRenderer.h"
#else
#include "Core/Platform.h"
#endif
#include <memory>

using Microsoft::WRL::ComPtr;

class GraphicsEngine;
class World;

namespace SparkEditor
{
    class EditorUI;

    /**
 * @brief Scene view panel
 * 
 * Renders the 3D scene with editor controls, gizmos, and selection.
 */
    class SceneViewPanel : public EditorPanel
    {
      public:
        /**
     * @brief Constructor
     */
        SceneViewPanel();

        /**
     * @brief Destructor
     */
        ~SceneViewPanel() override = default;

        /**
     * @brief Initialize the scene view panel
     * @return true if initialization succeeded
     */
        bool Initialize() override;

        /**
     * @brief Update scene view panel
     * @param deltaTime Time elapsed since last update
     */
        void Update(float deltaTime) override;

        /**
     * @brief Render scene view panel
     */
        void Render() override;

        /**
     * @brief Shutdown the scene view panel
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
     * @brief Set DirectX device for rendering
     * @param device DirectX device
     * @param context DirectX context
     */
        void SetDevice(ID3D11Device* device, ID3D11DeviceContext* context);

#ifdef _WIN32
        /// @brief Give the panel access to the editor's attach-mode GraphicsEngine
        /// (owned by EditorUI, non-owning here) so it can drive
        /// Spark::RenderWorldBasic() to render an ECS World into the viewport.
        void SetGraphics(GraphicsEngine* graphics) { m_graphics = graphics; }
#endif

        /// @brief Inject the shared ECS World to render (owned by EditorUI; the
        /// panel holds a non-owning pointer). Unit C1: the panel no longer owns
        /// its own demo World — EditorUI owns the single live document World.
        /// Any in-flight gizmo drag references the previous World, so it is
        /// abandoned here (SwapWorld() clears the CommandHistory separately).
        void SetWorld(World* world)
        {
            m_world = world;
            // Opening/replacing a document happens while the Project Browser
            // modal still owns focus.  ImGui can consequently leave this dock
            // node with no selected tab: its background looks like a black
            // viewport, but SceneViewPanel::RenderSceneContent() never runs.
            // Request focus once on the next frame so the first opened scene
            // is visible immediately.  Once this panel has rendered an active
            // frame, later World swaps (including play-mode rollback) preserve
            // whichever viewport tab the user selected.
            if (!m_hasRenderedActiveFrame)
                m_requestFocusOnNextRender = true;
            m_gizmoDragging = false;
            m_gizmoDragAxis = -1;
            m_gizmoDragEntity = entt::null;
        }

        /// @brief Wire the EditorUI document-selection sink (same Unit C2
        /// pattern as HierarchyPanel::SetSelectionSink). Supplies the selected
        /// World entity for the W9 viewport tools (translate gizmo, duplicate,
        /// align-to-ground, focus). Tools stay disabled while unwired.
        void SetSelectionSink(EditorUI* sink) { m_selectionSink = sink; }

        /// @brief Set collaborative session for peer visualization in viewport
        void SetCollabSession(CollaborativeEditSession* session) { m_collabSession = session; }

      private:
        void RenderToolbar();
        void RenderSceneContent();
        void RenderPeerOverlays();
        void HandleInput();
        void UpdateCamera(float deltaTime);
        void CreateRenderTexture(int width, int height);

        // W9 scene-edit tools (translate gizmo / duplicate / align / focus)
        void HandleEditShortcuts();
        ::EntityID GetSelectedWorldEntity() const;
        void FocusSelectedEntity();
        void DuplicateSelectedEntity();
        void AlignSelectedEntityToGround();
#ifdef _WIN32
        void ComputeCameraMatrices(DirectX::XMMATRIX& view, DirectX::XMMATRIX& proj) const;
        void RenderTranslateGizmo(float imgX, float imgY, float imgW, float imgH, bool viewportHovered);
#endif

      private:
        // Rendering resources
        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11DeviceContext> m_context;
        ComPtr<ID3D11Texture2D> m_renderTarget;
        ComPtr<ID3D11RenderTargetView> m_rtv;
        ComPtr<ID3D11ShaderResourceView> m_srv;
        ComPtr<ID3D11Texture2D> m_depthTexture;
        ComPtr<ID3D11DepthStencilView> m_dsv;

#ifdef _WIN32
        // Non-owning: GraphicsEngine attached to the editor's device, owned by
        // EditorUI. Null until EditorUI::SetGraphicsDevice() has run.
        GraphicsEngine* m_graphics = nullptr;
#endif

        // Non-owning: the single live ECS World (the document being edited),
        // owned by EditorUI. Null until EditorUI::SetGraphicsDevice() has run
        // and called SetWorld(). The mesh cache stays panel-side (a rendering
        // concern) even though the World itself moved to EditorUI.
        World* m_world = nullptr;

#ifdef _WIN32
        Spark::WorldMeshCache m_meshCache;
        Spark::WorldBasicRenderStats m_lastRenderStats;
#endif

        // Camera controls
        float m_cameraDistance = 10.0f;
        float m_cameraYaw = 0.0f;
        float m_cameraPitch = 0.0f;
        float m_cameraSpeed = 5.0f;
        // Orbit focus point: eye = m_cameraTarget - forward(yaw,pitch) * m_cameraDistance.
        // WASD/QE (UpdateCamera) pans this point; right-drag/wheel (HandleInput)
        // orbit/zoom around it (see RenderSceneContent()).
        DirectX::XMFLOAT3 m_cameraTarget = {0.0f, 1.0f, 0.0f};

        // Scene state
        bool m_showGrid = true;
        bool m_showGizmos = true;
        bool m_requestFocusOnNextRender = true;
        bool m_hasRenderedActiveFrame = false;
        int m_renderTextureWidth = 512;
        int m_renderTextureHeight = 512;

        // Render mode
        enum class RenderMode
        {
            Shaded,
            Wireframe,
            Unlit,
            Normals,
            Depth
        };
        RenderMode m_renderMode = RenderMode::Shaded;

        // Gizmo mode
        enum class GizmoMode
        {
            Move,
            Rotate,
            Scale
        };
        GizmoMode m_gizmoMode = GizmoMode::Move;

        // Collaborative peer visualization
        CollaborativeEditSession* m_collabSession = nullptr;

        // SelectionManager integration — tracks the primary selected entity
        // so the scene view can render gizmo highlights on it.
        uint64_t m_selectedEntityId = 0;       ///< Primary selected entity (0 = none).
        uint32_t m_selectionMgrCallbackId = 0; ///< SelectionManager subscription handle.

        // ── W9 scene-edit tools ─────────────────────────────────────────────
        // Non-owning; supplies the World-mode selected entity
        // (EditorUI::GetSelectedEntity) — the document selection the
        // Hierarchy publishes and the Inspector consumes.
        EditorUI* m_selectionSink = nullptr;

        bool m_snapEnabled = false; ///< Grid snap applied to gizmo translation drags.
        float m_snapSize = 1.0f;    ///< Snap size in meters (0.25 / 0.5 / 1 / 2).

        // Translate-gizmo drag state (Move mode). Captures entity id + start
        // snapshots — never component pointers, which EnTT invalidates on
        // storage growth.
        bool m_gizmoDragging = false;
        int m_gizmoDragAxis = -1;  ///< 0=X 1=Y 2=Z while dragging, else -1.
        int m_gizmoHoverAxis = -1; ///< Axis under the mouse this frame, else -1.
        ::EntityID m_gizmoDragEntity = entt::null;
        DirectX::XMFLOAT3 m_dragStartLocalPos = {0.0f, 0.0f, 0.0f};
        DirectX::XMFLOAT3 m_dragStartWorldPos = {0.0f, 0.0f, 0.0f};
        float m_dragStartMouseX = 0.0f;
        float m_dragStartMouseY = 0.0f;
    };

} // namespace SparkEditor
