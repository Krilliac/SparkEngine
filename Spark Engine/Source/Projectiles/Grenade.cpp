// Grenade.cpp
#include "Grenade.h"
#include "Utils/Assert.h"

using DirectX::XMMATRIX;
using DirectX::XMFLOAT3;

Grenade::Grenade()
    : m_fuseTime(3.0f)
    , m_explosionRadius(8.0f)
    , m_hasExploded(false)
{
    // Validate parameters
    ASSERT_MSG(m_fuseTime > 0.0f, "Grenade fuse time must be positive");
    ASSERT_MSG(m_explosionRadius > 0.0f, "Grenade explosion radius must be positive");

    m_damage = 100.0f;
    m_speed = 15.0f;
    m_maxLifeTime = 5.0f;

    // Enable gravity
    SetGravity(true, 1.0f);

    // Scale grenade
    XMFLOAT3 scale{ 0.3f, 0.3f, 0.3f };
    ASSERT_MSG(scale.x > 0.0f && scale.y > 0.0f && scale.z > 0.0f, "Grenade scale must be positive");
    SetScale(scale);
}

HRESULT Grenade::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    ASSERT_MSG(device != nullptr, "Grenade::Initialize device is null");
    ASSERT_MSG(context != nullptr, "Grenade::Initialize context is null");

    HRESULT hr = Projectile::Initialize(device, context);
    ASSERT_MSG(SUCCEEDED(hr), "Projectile::Initialize failed in Grenade");
    return hr;
}

void Grenade::Update(float deltaTime)
{
    ASSERT_MSG(deltaTime >= 0.0f && std::isfinite(deltaTime), "Invalid deltaTime in Grenade::Update");

    if (!m_active) return;

    // Use base physics/lifetime/collision (this also increments m_lifeTime)
    Projectile::Update(deltaTime);

    // Check fuse after base update (m_lifeTime is already incremented by Projectile::Update)
    if (m_lifeTime >= m_fuseTime && !m_hasExploded)
    {
        Explode();
        return;
    }
}

void Grenade::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    if (!m_active) return;
    ASSERT_MSG(m_mesh != nullptr, "Grenade mesh not initialized");
    Projectile::Render(view, projection);
}

void Grenade::Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed)
{
    m_hasExploded = false;
    Projectile::Fire(startPosition, direction, speed);
}

void Grenade::Explode()
{
    if (m_hasExploded) return;
    m_hasExploded = true;

    // TODO: spawn explosion effect at current position
    // TODO: apply area damage using m_explosionRadius

    Deactivate();
}