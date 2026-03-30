#include "Core/Platform.h"
// Grenade.cpp
#include "Grenade.h"
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include "Utils/LogMacros.h"
#include "Physics/PhysicsSystem.h"

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;


Grenade::Grenade() : m_fuseTime(3.0f), m_explosionRadius(8.0f), m_hasExploded(false)
{
    // Validate parameters
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_fuseTime > 0.0f, "Grenade fuse time must be positive");
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_explosionRadius > 0.0f, "Grenade explosion radius must be positive");

    m_damage = 100.0f;
    m_speed = 15.0f;
    m_maxLifeTime = 5.0f;

    // Enable gravity
    SetGravity(true, 1.0f);

    // Scale grenade
    XMFLOAT3 scale{0.3f, 0.3f, 0.3f};
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, scale.x > 0.0f && scale.y > 0.0f && scale.z > 0.0f,
                      "Grenade scale must be positive");
    SetScale(scale);
}

HRESULT Grenade::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, context);

    HRESULT hr = Projectile::Initialize(device, context);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Projectile::Initialize failed in Grenade");
    return hr;
}

void Grenade::Update(float deltaTime)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, deltaTime >= 0.0f && std::isfinite(deltaTime),
                      "Invalid deltaTime in Grenade::Update");

    if (!m_active)
        return;

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
    if (!m_active)
        return;
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Game, m_mesh);
    Projectile::Render(view, projection);
}

void Grenade::Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Grenade thrown: fuse=%.1fs, radius=%.1f", m_fuseTime, m_explosionRadius);
    m_hasExploded = false;
    Projectile::Fire(startPosition, direction, speed);
}

void Grenade::Explode()
{
    if (m_hasExploded)
        return;
    m_hasExploded = true;

    XMFLOAT3 position = GetPosition();
    SPARK_LOG_INFO(Spark::LogCategory::Game, "Grenade detonated at (%.1f, %.1f, %.1f)", position.x, position.y,
                   position.z);

    // Apply area damage to all physics bodies within explosion radius
    if (m_physicsSystem)
    {
        std::vector<PhysicsBody*> hitBodies;
        if (m_physicsSystem->SphereOverlap(position, m_explosionRadius, hitBodies))
        {
            for (PhysicsBody* body : hitBodies)
            {
                if (!body)
                    continue;

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
                    XMFLOAT3 impulseDir = {(dx / distance) * impulseStrength,
                                           (dy / distance + 0.7f) * impulseStrength, // Higher upward bias than rocket
                                           (dz / distance) * impulseStrength};
                    body->ApplyImpulse(impulseDir);
                }
            }
        }
    }

    Deactivate();
}
