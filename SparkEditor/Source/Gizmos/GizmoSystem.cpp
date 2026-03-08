#include "GizmoSystem.h"

using namespace DirectX;
namespace SparkEditor
{

    // --- Ray ---

    Ray Ray::ScreenToWorldRay(float /*screenX*/, float /*screenY*/, const XMMATRIX& /*viewMatrix*/,
                              const XMMATRIX& /*projMatrix*/, const XMFLOAT4& /*viewport*/)
    {
        Ray ray;
        ray.origin = {0.0f, 0.0f, 0.0f};
        ray.direction = {0.0f, 0.0f, 1.0f};
        return ray;
    }

    // --- GizmoSystem ---

    GizmoSystem::GizmoSystem() = default;

    GizmoSystem::~GizmoSystem() = default;

    bool GizmoSystem::Initialize(ID3D11Device* /*device*/, ID3D11DeviceContext* /*context*/)
    {
        return false;
    }

    void GizmoSystem::Shutdown() {}

    void GizmoSystem::Update(float /*deltaTime*/) {}

    void GizmoSystem::Render(const std::vector<Transform*>& /*selectedObjects*/, const XMMATRIX& /*viewMatrix*/,
                             const XMMATRIX& /*projMatrix*/, const XMFLOAT4& /*viewport*/)
    {
    }

    bool GizmoSystem::HandleMouseInput(float /*mouseX*/, float /*mouseY*/, bool /*isMouseDown*/,
                                       const XMMATRIX& /*viewMatrix*/, const XMMATRIX& /*projMatrix*/,
                                       const XMFLOAT4& /*viewport*/, std::vector<Transform*>& /*selectedObjects*/)
    {
        return false;
    }

    // Private methods

    void GizmoSystem::RenderTranslationGizmo(const Transform& /*transform*/, const XMMATRIX& /*viewMatrix*/,
                                             const XMMATRIX& /*projMatrix*/)
    {
    }

    void GizmoSystem::RenderRotationGizmo(const Transform& /*transform*/, const XMMATRIX& /*viewMatrix*/,
                                          const XMMATRIX& /*projMatrix*/)
    {
    }

    void GizmoSystem::RenderScaleGizmo(const Transform& /*transform*/, const XMMATRIX& /*viewMatrix*/,
                                       const XMMATRIX& /*projMatrix*/)
    {
    }

    GizmoAxis GizmoSystem::TestTranslationGizmoHit(const Ray& /*ray*/, const Transform& /*transform*/)
    {
        return GizmoAxis::NONE;
    }

    GizmoAxis GizmoSystem::TestRotationGizmoHit(const Ray& /*ray*/, const Transform& /*transform*/)
    {
        return GizmoAxis::NONE;
    }

    GizmoAxis GizmoSystem::TestScaleGizmoHit(const Ray& /*ray*/, const Transform& /*transform*/)
    {
        return GizmoAxis::NONE;
    }

    void GizmoSystem::ApplyTranslation(const XMFLOAT3& /*delta*/, std::vector<Transform*>& /*transforms*/) {}

    void GizmoSystem::ApplyRotation(GizmoAxis /*axis*/, float /*angleDelta*/, std::vector<Transform*>& /*transforms*/)
    {
    }

    void GizmoSystem::ApplyScale(const XMFLOAT3& /*scale*/, GizmoAxis /*axis*/, std::vector<Transform*>& /*transforms*/)
    {
    }

    float GizmoSystem::SnapToGrid(float /*value*/) const
    {
        return 0.0f;
    }

    float GizmoSystem::SnapToRotation(float /*angle*/) const
    {
        return 0.0f;
    }

    XMFLOAT3 GizmoSystem::CalculateGizmoCenter(const std::vector<Transform*>& /*transforms*/) const
    {
        return {0.0f, 0.0f, 0.0f};
    }

    float GizmoSystem::CalculateAdaptiveSize(const XMFLOAT3& /*gizmoPosition*/, const XMMATRIX& /*viewMatrix*/) const
    {
        return 1.0f;
    }

    XMFLOAT4 GizmoSystem::GetAxisColor(GizmoAxis /*axis*/, bool /*isHighlighted*/, bool /*isSelected*/) const
    {
        return {1.0f, 1.0f, 1.0f, 1.0f};
    }

    bool GizmoSystem::CreateGizmoGeometry()
    {
        return false;
    }

    bool GizmoSystem::CreateGizmoShaders()
    {
        return false;
    }

} // namespace SparkEditor
