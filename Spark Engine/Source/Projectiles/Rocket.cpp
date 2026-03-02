// Rocket.cpp
#include "Rocket.h"
#include "Utils/Assert.h"
#include "..\Utils\MathUtils.h"

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;

Rocket::Rocket()
    : m_explosionRadius(5.0f)
    , m_hasExploded(false)
{
    ASSERT_MSG(m_explosionRadius > 0.0f, "Rocket explosion radius must be positive");

    m_damage = 75.0f;
    m_speed = 30.0f;
    m_maxLifeTime = 10.0f;

    // Enable gravity with reduced scale
    SetGravity(true, 0.3f);

    XMFLOAT3 scale{ 0.2f, 0.2f, 0.8f };
    ASSERT_MSG(scale.x > 0 && scale.y > 0 && scale.z > 0, "Rocket scale must be positive");
    SetScale(scale);
}

HRESULT Rocket::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT_MSG(device != nullptr, "Rocket::Initialize device is null");
    ASSERT_MSG(context != nullptr, "Rocket::Initialize context is null");

    HRESULT hr = Projectile::Initialize(device, context);
    ASSERT_MSG(SUCCEEDED(hr), "Projectile::Initialize failed in Rocket");
    return hr;
}

void Rocket::Update(float deltaTime)
{
    ASSERT_MSG(deltaTime >= 0.0f && std::isfinite(deltaTime), "Invalid deltaTime in Rocket::Update");
    Projectile::Update(deltaTime);
    // TODO: add trail effects
}

void Rocket::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    if (!m_active) return;
    ASSERT_MSG(m_mesh != nullptr, "Rocket mesh not initialized");
    Projectile::Render(view, projection);
}

void Rocket::Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed)
{
    m_hasExploded = false;
    Projectile::Fire(startPosition, direction, speed);
}

void Rocket::OnHit(GameObject* target)
{
    ASSERT_MSG(target != nullptr, "Rocket::OnHit target is null");
    if (!m_hasExploded)
        Explode(GetPosition());
}

void Rocket::OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal)
{
    ASSERT_MSG(std::isfinite(hitPoint.x) && std::isfinite(hitPoint.y) && std::isfinite(hitPoint.z),
        "Invalid hitPoint in Rocket::OnHitWorld");
    if (!m_hasExploded)
        Explode(hitPoint);
}

void Rocket::Explode(const XMFLOAT3& position)
{
    ASSERT_MSG(!m_hasExploded, "Rocket exploded multiple times");
    m_hasExploded = true;

    // TODO: spawn explosion effect at `position`
    // TODO: apply area damage using m_explosionRadius

    Deactivate();
}