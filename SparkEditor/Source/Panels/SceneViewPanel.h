/**
 * @file SceneViewPanel.h
 * @brief Scene view panel for 3D scene rendering in the Spark Engine Editor
 * @author Spark Engine Team
 * @date 2025
 */

#pragma once

#include "../Core/EditorPanel.h"
#ifdef _WIN32
#include <d3d11.h>
#include <wrl/client.h>
#else
#include "Core/Platform.h"
#endif
#include <memory>

using Microsoft::WRL::ComPtr;

namespace SparkEditor
{

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

      private:
        void RenderToolbar();
        void RenderSceneContent();
        void HandleInput();
        void UpdateCamera(float deltaTime);
        void CreateRenderTexture(int width, int height);

      private:
        // Rendering resources
        ComPtr<ID3D11Device> m_device;
        ComPtr<ID3D11DeviceContext> m_context;
        ComPtr<ID3D11Texture2D> m_renderTarget;
        ComPtr<ID3D11RenderTargetView> m_rtv;
        ComPtr<ID3D11ShaderResourceView> m_srv;

        // Camera controls
        float m_cameraDistance = 10.0f;
        float m_cameraYaw = 0.0f;
        float m_cameraPitch = 0.0f;
        float m_cameraSpeed = 5.0f;

        // Scene state
        bool m_showGrid = true;
        bool m_showGizmos = true;
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
    };

} // namespace SparkEditor