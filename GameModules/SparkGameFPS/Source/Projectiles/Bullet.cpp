#include "Core/Platform.h"
// Bullet.cpp
#include "Bullet.h"
#include "Utils/Assert.h"
#include "Utils/Validate.h"
#include "Utils/LogMacros.h"

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
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, scale.x > 0 && scale.y > 0 && scale.z > 0,
                      "Bullet scale must be positive");
    SetScale(scale);
}

HRESULT Bullet::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    SPARK_TRACE_ENTER(Spark::LogCategory::Game);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, device);
    SPARK_REQUIRE_NOT_NULL(Spark::LogCategory::Game, context);

    // Base initialization sets up mesh and transforms
    HRESULT hr = Projectile::Initialize(device, context);
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, SUCCEEDED(hr), "Projectile::Initialize failed in Bullet");
    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Bullet initialized (damage=%.0f, speed=%.0f)", m_damage, m_speed);
    return hr;
}

void Bullet::Update(float deltaTime)
{
    SPARK_REQUIRE_MSG(Spark::LogCategory::Game, deltaTime >= 0.0f && std::isfinite(deltaTime),
                      "Invalid deltaTime in Bullet::Update");
    // Use base physics/lifetime/collision
    Projectile::Update(deltaTime);
}

void Bullet::Render(const XMMATRIX& view, const XMMATRIX& projection)
{
    if (!m_active)
        return;
    SPARK_VALIDATE_NOT_NULL(Spark::LogCategory::Game, m_mesh);
    Projectile::Render(view, projection);
}
