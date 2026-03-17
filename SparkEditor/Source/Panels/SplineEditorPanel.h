/**
 * @file SplineEditorPanel.h
 * @brief Spline path editor panel for the Spark Engine Editor
 */

#pragma once

#include "../Core/EditorPanel.h"
#ifdef _WIN32
#include <DirectXMath.h>
#else
#include "Core/Platform.h"
#endif
using namespace DirectX;
#include <vector>

namespace SparkEditor
{

    /**
     * @brief Panel for creating and editing spline paths
     *
     * Allows adding/removing/moving control points for Bezier
     * and Catmull-Rom splines used by SplineComponent.
     */
    class SplineEditorPanel : public EditorPanel
    {
      public:
        SplineEditorPanel();
        ~SplineEditorPanel() override = default;

        bool Initialize() override;
        void Update(float deltaTime) override;
        void Render() override;
        void Shutdown() override;

      private:
        struct ControlPoint
        {
            XMFLOAT3 position = {0.0f, 0.0f, 0.0f};
        };

        void RenderPointList();
        void RenderPointEditor();
        void RenderSplineSettings();

        std::vector<ControlPoint> m_points;
        int m_selectedPoint = -1;
        int m_splineType = 0; // 0=Catmull-Rom, 1=Bezier
        bool m_closedLoop = false;
        bool m_showTangents = true;
        float m_tension = 0.5f;
    };

} // namespace SparkEditor
