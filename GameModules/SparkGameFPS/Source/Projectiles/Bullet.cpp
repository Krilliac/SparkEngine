#include "Bullet.h"
#include "Core/FPSLog.h"
#include "Core/Platform.h"
// Bullet.cpp
#include "Core/FPSAssert.h"

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

Bullet::Bullet()
{
    // Customize bullet parameters
    m_damage = 15.0f;
    m_speed = 100.0f;
    m_maxLifeTime = 3.0f;

    // Validate scale values are positive
    XMFLOAT3 scale = {0.05f, 0.05f, 0.2f};
    FPS_REQUIRE_MSG(scale.x > 0 && scale.y > 0 && scale.z > 0, "Bullet scale must be positive");
    SetScale(scale);
}

HRESULT Bullet::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    FPS_REQUIRE_NOT_NULL(device);
    FPS_REQUIRE_NOT_NULL(context);

    // Base initialization sets up mesh and transforms
    HRESULT hr = Projectile::Initialize(device, context);
    FPS_REQUIRE_MSG(SUCCEEDED(hr), "Projectile::Initialize failed in Bullet");
    FPS_LOG_DEBUG("Bullet initialized (damage={:.0f}, speed={:.0f})", m_damage, m_speed);
    return hr;
}

void Bullet::Update(float deltaTime)
{
    FPS_REQUIRE_MSG(deltaTime >= 0.0f && std::isfinite(deltaTime), "Invalid deltaTime in Bullet::Update");
    // Use base physics/lifetime/collision
    Projectile::Update(deltaTime);
}

void Bullet::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    if (!m_active)
        return;
    if (m_mesh == nullptr)
    {
        FPS_LOG_ERROR("{}: 'm_mesh' must not be null", __func__);
        return;
    }
    Projectile::Render(view, projection);
}
