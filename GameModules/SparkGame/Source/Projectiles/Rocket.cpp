#include "Core/Platform.h"
// Rocket.cpp
#include "Rocket.h"
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include "Utils/MathUtils.h"
#include "Physics/PhysicsSystem.h"

using DirectX::XMFLOAT3;
using DirectX::XMMATRIX;


Rocket::Rocket() : m_explosionRadius(5.0f), m_hasExploded(false), m_trailTimer(0.0f)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, m_explosionRadius > 0.0f, "Rocket explosion radius must be positive");

    m_damage = 75.0f;
    m_speed = 30.0f;
    m_maxLifeTime = 10.0f;

    // Enable gravity with reduced scale
    SetGravity(true, 0.3f);

    XMFLOAT3 scale{0.2f, 0.2f, 0.8f};
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, scale.x > 0 && scale.y > 0 && scale.z > 0,
                      "Rocket scale must be positive");
    SetScale(scale);
}

HRESULT Rocket::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, context);

    HRESULT hr = Projectile::Initialize(device, context);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Projectile::Initialize failed in Rocket");
    return hr;
}

void Rocket::Update(float deltaTime)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, deltaTime >= 0.0f && std::isfinite(deltaTime),
                      "Invalid deltaTime in Rocket::Update");
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
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Game, m_mesh);
    Projectile::Render(view, projection);
}

void Rocket::Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    m_hasExploded = false;
    m_trailPositions.clear();
    m_trailTimer = 0.0f;
    Projectile::Fire(startPosition, direction, speed);
}

void Rocket::OnHit(GameObject* target)
{
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, target);
    if (!m_hasExploded)
        Explode(GetPosition());
}

void Rocket::OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game,
                      std::isfinite(hitPoint.x) && std::isfinite(hitPoint.y) && std::isfinite(hitPoint.z),
                      "Invalid hitPoint in Rocket::OnHitWorld");
    if (!m_hasExploded)
        Explode(hitPoint);
}

void Rocket::Explode(const XMFLOAT3& position)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, !m_hasExploded, "Rocket exploded multiple times");
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
