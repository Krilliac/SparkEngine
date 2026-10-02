#include "Rocket.h"
#include "Core/FPSLog.h"
#include "Core/Platform.h"
// Rocket.cpp
#include "Core/FPSAssert.h"
#include "Physics/PhysicsSystem.h"

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;


Rocket::Rocket() : m_explosionRadius(5.0f), m_hasExploded(false), m_trailTimer(0.0f)
{
    FPS_REQUIRE_MSG(m_explosionRadius > 0.0f, "Rocket explosion radius must be positive");

    m_damage = 75.0f;
    m_speed = 30.0f;
    m_maxLifeTime = 10.0f;

    // Enable gravity with reduced scale
    SetGravity(true, 0.3f);

    XMFLOAT3 scale{0.2f, 0.2f, 0.8f};
    FPS_REQUIRE_MSG(scale.x > 0 && scale.y > 0 && scale.z > 0, "Rocket scale must be positive");
    SetScale(scale);
}

HRESULT Rocket::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    FPS_REQUIRE_NOT_NULL(device);
    FPS_REQUIRE_NOT_NULL(context);

    HRESULT hr = Projectile::Initialize(device, context);
    FPS_REQUIRE_MSG(SUCCEEDED(hr), "Projectile::Initialize failed in Rocket");
    return hr;
}

void Rocket::Update(float deltaTime)
{
    FPS_REQUIRE_MSG(deltaTime >= 0.0f && std::isfinite(deltaTime), "Invalid deltaTime in Rocket::Update");
    Projectile::Update(deltaTime);

    // Trail effect: record positions at intervals for visual trail rendering
    if (m_active)
    {
        m_trailTimer += deltaTime;
        if (m_trailTimer >= 0.016f) // ~60Hz trail point emission
        {
            m_trailTimer = 0.0f;
            m_trailPositions.push_back(GetPosition());
            // Keep trail length bounded
            if (m_trailPositions.size() > 30)
                m_trailPositions.erase(m_trailPositions.begin());
        }
    }
}

void Rocket::Render(const XMMATRIX& view, const XMMATRIX& projection)
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

void Rocket::Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed)
{
    FPS_LOG_DEBUG("Rocket launched: speed={:.1f}, radius={:.1f}", speed, m_explosionRadius);
    m_hasExploded = false;
    m_trailPositions.clear();
    m_trailTimer = 0.0f;
    Projectile::Fire(startPosition, direction, speed);
}

void Rocket::OnHit(GameObject* target)
{
    FPS_REQUIRE_NOT_NULL(target);
    if (!m_hasExploded)
        Explode(GetPosition());
}

void Rocket::OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal)
{
    FPS_REQUIRE_MSG(std::isfinite(hitPoint.x) && std::isfinite(hitPoint.y) && std::isfinite(hitPoint.z),
                    "Invalid hitPoint in Rocket::OnHitWorld");
    if (!m_hasExploded)
        Explode(hitPoint);
}

void Rocket::Explode(const XMFLOAT3& position)
{
    FPS_REQUIRE_MSG(!m_hasExploded, "Rocket exploded multiple times");
    FPS_LOG_INFO("Rocket exploded at ({:.1f}, {:.1f}, {:.1f})", position.x, position.y, position.z);
    m_hasExploded = true;

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

                // Calculate distance-based damage falloff
                XMFLOAT3 bodyPos = body->GetPosition();
                float dx = bodyPos.x - position.x;
                float dy = bodyPos.y - position.y;
                float dz = bodyPos.z - position.z;
                float distance = sqrtf(dx * dx + dy * dy + dz * dz);
                float falloff = 1.0f - std::clamp(distance / m_explosionRadius, 0.0f, 1.0f);
                float appliedDamage = m_damage * falloff;

                // Apply explosive impulse pushing bodies away from center
                if (distance > 0.001f)
                {
                    float impulseStrength = appliedDamage * 2.0f * falloff;
                    XMFLOAT3 impulseDir = {(dx / distance) * impulseStrength,
                                           (dy / distance + 0.5f) * impulseStrength, // Upward bias
                                           (dz / distance) * impulseStrength};
                    body->ApplyImpulse(impulseDir);
                }
            }
        }
    }

    m_trailPositions.clear();
    Deactivate();
}
