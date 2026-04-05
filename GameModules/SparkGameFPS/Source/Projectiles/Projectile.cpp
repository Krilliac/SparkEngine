#include "Projectile.h"
#include "Core/Platform.h"
// Projectile.cpp
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include "Utils/MathUtils.h"
#include "Utils/LogMacros.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif // SPARK_PLATFORM_WINDOWS

using namespace DirectX;

Projectile::Projectile()
    : m_velocity{0, 0, 0}, m_speed(50.0f), m_lifeTime(0.0f), m_maxLifeTime(5.0f), m_damage(25.0f), m_active(false),
      m_boundingSphere(GetPosition(), 0.1f), m_hasGravity(false), m_gravityScale(1.0f)
{
    // Base GameObject scale
    XMFLOAT3 scale{0.1f, 0.1f, 0.3f};
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, scale.x > 0 && scale.y > 0 && scale.z > 0, "Scale must be positive");
    SetScale(scale);
}

Projectile::~Projectile() = default;

HRESULT Projectile::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, context);

    HRESULT hr = GameObject::Initialize(device, context);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "GameObject::Initialize failed in Projectile");
    if (FAILED(hr))
        return hr;

    UpdateBoundingSphere();
    return S_OK;
}

void Projectile::Update(float deltaTime)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, deltaTime >= 0 && std::isfinite(deltaTime), "Invalid deltaTime");
    if (!m_active)
        return;

    // Physics integration
    UpdatePhysics(deltaTime);

    // Move
    XMFLOAT3 delta{m_velocity.x * deltaTime, m_velocity.y * deltaTime, m_velocity.z * deltaTime};
    Translate(delta);

    // Lifetime
    m_lifeTime += deltaTime;
    if (m_lifeTime >= m_maxLifeTime)
    {
        Deactivate();
        return;
    }

    // Collision
    CheckCollisions();

    // Update transform
    GameObject::Update(deltaTime);

    // Update bounding volume
    UpdateBoundingSphere();
}

void Projectile::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    if (!m_active)
        return;
    GameObject::Render(view, projection);
}

void Projectile::Fire(const XMFLOAT3& startPosition, const XMFLOAT3& direction, float speed)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, speed >= 0, "Speed must be non-negative");
    SetPosition(startPosition);
    m_speed = speed;

    XMVECTOR dirV = XMVector3Normalize(XMLoadFloat3(&direction));
    XMVECTOR velV = XMVectorScale(dirV, speed);
    XMStoreFloat3(&m_velocity, velV);

    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Projectile fired: speed=%.1f, pos=(%.1f, %.1f, %.1f)", speed,
                    startPosition.x, startPosition.y, startPosition.z);
    m_lifeTime = 0.0f;
    m_active = true;
    SetActive(true);
    SetVisible(true);

    UpdateBoundingSphere();
}

void Projectile::Deactivate()
{
    m_active = false;
    SetActive(false);
    SetVisible(false);
    m_lifeTime = 0.0f;
    m_velocity = XMFLOAT3{0, 0, 0};
}

void Projectile::Reset()
{
    Deactivate();
    SetPosition(XMFLOAT3{0, 0, 0});
    SetRotation(XMFLOAT3{0, 0, 0});
}

void Projectile::OnHit(GameObject* target)
{
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, target);
    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Projectile hit target, damage=%.1f", m_damage);
    Deactivate();
}

void Projectile::OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal)
{
    // normal not validated
    Deactivate();
}

void Projectile::SetGravity(bool enabled, float scale)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, scale >= 0, "Gravity scale must be non-negative");
    m_hasGravity = enabled;
    m_gravityScale = scale;
}

void Projectile::ApplyForce(const XMFLOAT3& force)
{
    XMVECTOR v = XMLoadFloat3(&m_velocity);
    XMVECTOR f = XMLoadFloat3(&force);
    XMVECTOR sum = XMVectorAdd(v, f);
    XMStoreFloat3(&m_velocity, sum);
}

void Projectile::CreateMesh()
{
    if (m_mesh)
        m_mesh->CreateSphere(0.1f, 8, 8);
}

void Projectile::CheckCollisions()
{
    // Example: ground plane at y=0
    if (GetPosition().y < 0.0f)
    {
        OnHitWorld(GetPosition(), XMFLOAT3{0, 1, 0});
    }
}

void Projectile::UpdatePhysics(float deltaTime)
{
    if (m_hasGravity)
        m_velocity.y += -9.8f * m_gravityScale * deltaTime;

    // Frame-rate independent drag
    XMVECTOR v = XMLoadFloat3(&m_velocity);
    v = XMVectorScale(v, powf(0.98f, deltaTime * 60.0f));
    XMStoreFloat3(&m_velocity, v);
}

void Projectile::UpdateBoundingSphere()
{
    m_boundingSphere.Center = GetPosition();
    // radius remains unchanged
}
