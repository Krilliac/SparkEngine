#include "Core/Platform.h"
// Bullet.cpp
#include "Bullet.h"
#include "Utils/Assert.h"

using DirectX::XMMATRIX;
using DirectX::XMFLOAT3;

Bullet::Bullet()
{
    // Customize bullet parameters
    m_damage = 15.0f;
    m_speed = 100.0f;
    m_maxLifeTime = 3.0f;

    // Assert scale values are positive
    XMFLOAT3 scale = { 0.05f, 0.05f, 0.2f };
    ASSERT_MSG(scale.x > 0 && scale.y > 0 && scale.z > 0, "Bullet scale must be positive");
    SetScale(scale);
}

HRESULT Bullet::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT_MSG(device != nullptr, "Bullet::Initialize - device is null");
    ASSERT_MSG(context != nullptr, "Bullet::Initialize - context is null");

    // Base initialization sets up mesh and transforms
    HRESULT hr = Projectile::Initialize(device, context);
    ASSERT_MSG(SUCCEEDED(hr), "Projectile::Initialize failed in Bullet");
    return hr;
}

void Bullet::Update(float deltaTime)
{
    ASSERT_MSG(deltaTime >= 0.0f && std::isfinite(deltaTime), "Invalid deltaTime in Bullet::Update");
    // Use base physics/lifetime/collision
    Projectile::Update(deltaTime);
}

void Bullet::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    if (!m_active) return;
    ASSERT_MSG(m_mesh != nullptr, "Bullet mesh not initialized");
    Projectile::Render(view, projection);
}
