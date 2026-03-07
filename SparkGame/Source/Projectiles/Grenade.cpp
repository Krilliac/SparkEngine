#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// Grenade.cpp
#include "Grenade.h"
#include "Utils/Assert.h"
#include "Physics/PhysicsSystem.h"

using DirectX::XMMATRIX;
using DirectX::XMFLOAT3;

// External physics system reference for area damage queries
extern PhysicsSystem* g_physicsSystem;

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

    XMFLOAT3 position = GetPosition();

    // Apply area damage to all physics bodies within explosion radius
    if (g_physicsSystem)
    {
        std::vector<PhysicsBody*> hitBodies;
        if (g_physicsSystem->SphereOverlap(position, m_explosionRadius, hitBodies))
        {
            for (PhysicsBody* body : hitBodies)
            {
                if (!body) continue;

                // Calculate distance-based damage falloff (grenades have larger radius, higher damage)
                XMFLOAT3 bodyPos = body->GetPosition();
                float dx = bodyPos.x - position.x;
                float dy = bodyPos.y - position.y;
                float dz = bodyPos.z - position.z;
                float distance = sqrtf(dx * dx + dy * dy + dz * dz);
                float falloff = 1.0f - std::clamp(distance / m_explosionRadius, 0.0f, 1.0f);

                // Grenade uses quadratic falloff for more concentrated center damage
                falloff = falloff * falloff;
                float appliedDamage = m_damage * falloff;

                // Apply explosive impulse pushing bodies away from center
                if (distance > 0.001f)
                {
                    float impulseStrength = appliedDamage * 3.0f * falloff;
                    XMFLOAT3 impulseDir = {
                        (dx / distance) * impulseStrength,
                        (dy / distance + 0.7f) * impulseStrength, // Higher upward bias than rocket
                        (dz / distance) * impulseStrength
                    };
                    body->ApplyImpulse(impulseDir);
                }
            }
        }
    }

    Deactivate();
}

#endif // SPARK_PLATFORM_WINDOWS
