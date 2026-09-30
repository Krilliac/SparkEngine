#include "ProjectilePool.h"
#include "Core/FPSLog.h"
#include "Core/Platform.h"
// ProjectilePool.cpp
#include "Bullet.h"
#include "Rocket.h"
#include "Grenade.h"
#include "Game/Enemy.h"
#include "Engine/Events/EventSystem.h"
#include "Core/FPSAssert.h"
#include <algorithm>
#include <iostream>
#include <memory>

using namespace DirectX;

ProjectilePool::ProjectilePool(size_t poolSize) : m_poolSize(poolSize)
{
    FPS_CONSOLE("ProjectilePool constructed with size " + std::to_string(poolSize), "INFO");
    FPS_REQUIRE_MSG(poolSize > 0, "ProjectilePool size must be positive");
    m_projectiles.reserve(poolSize);
}

ProjectilePool::~ProjectilePool()
{
    FPS_CONSOLE("ProjectilePool destructor called.", "INFO");
    Shutdown();
}

HRESULT ProjectilePool::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    FPS_CONSOLE("ProjectilePool::Initialize called.", "OPERATION");

    m_device = device;
    m_context = context;

    // A NullRHI / headless host has no D3D11 device. The pool is still populated so the
    // combat loop (firing, flight, collision, recycling) really runs; only each
    // projectile's GPU mesh is skipped and Render() becomes a no-op. Leaving the pool
    // empty instead would make every shot a silent no-op that still reports success.
    m_hasRenderResources = (device != nullptr) && (context != nullptr);
    if (!m_hasRenderResources)
    {
        FPS_CONSOLE("ProjectilePool: no D3D11 device - projectiles are simulated but not rendered.", "WARNING");
    }

    // Create projectiles based on pool size distribution
    size_t bulletsCount = m_poolSize / 2;                            // 50% bullets
    size_t rocketsCount = m_poolSize / 4;                            // 25% rockets
    size_t grenadesCount = m_poolSize - bulletsCount - rocketsCount; // remainder

    // Helper lambda
    auto makeAndStore = [&](auto TypeFactory, size_t count)
    {
        for (size_t i = 0; i < count; ++i)
        {
            auto p = TypeFactory();
            FPS_REQUIRE_MSG(p != nullptr, "Failed to create projectile");
            if (!p)
                continue;
            if (m_hasRenderResources && FAILED(p->Initialize(m_device, m_context)))
                continue;
            m_availableProjectiles.push(p.get());
            m_projectiles.push_back(std::move(p));
        }
    };

    makeAndStore([] { return std::make_unique<Bullet>(); }, bulletsCount);
    makeAndStore([] { return std::make_unique<Rocket>(); }, rocketsCount);
    makeAndStore([] { return std::make_unique<Grenade>(); }, grenadesCount);

    FPS_REQUIRE_MSG(m_projectiles.size() == m_poolSize, "Some projectiles failed to initialize");

    FPS_CONSOLE("ProjectilePool created " + std::to_string(m_projectiles.size()) + " projectiles.", "INFO");
    return S_OK;
}

void ProjectilePool::Update(float deltaTime)
{
    // **FIXED: Remove per-frame logging completely**
    FPS_REQUIRE_MSG(deltaTime >= 0.0f && std::isfinite(deltaTime), "Invalid deltaTime in ProjectilePool::Update");

    for (auto& up : m_projectiles)
    {
        if (up->IsActive())
        {
            up->Update(deltaTime);
            if (!up->IsActive())
                ReturnProjectile(up.get());
        }
    }
}

void ProjectilePool::Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& projection)
{
    // **FIXED: Remove per-frame logging completely**
    // Device-less pools carry no projectile mesh, so there is nothing to draw.
    if (!m_hasRenderResources)
        return;

    for (auto& up : m_projectiles)
    {
        if (up->IsActive())
            up->Render(view, projection);
    }
}

void ProjectilePool::Shutdown()
{
    FPS_LOG_INFO("ProjectilePool shutting down");
    FPS_CONSOLE("ProjectilePool::Shutdown called.", "OPERATION");

    m_projectiles.clear();
    std::queue<Projectile*> empty;
    std::swap(m_availableProjectiles, empty);
    m_hasRenderResources = false;
    m_device = nullptr;
    m_context = nullptr;

    FPS_CONSOLE("ProjectilePool shutdown complete.", "INFO");
}

Projectile* ProjectilePool::GetProjectile()
{
    // **FIXED: Rate-limited logging for projectile acquisition**
    FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool::GetProjectile called.", "OPERATION");
    if (m_availableProjectiles.empty())
    {
        FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool: No available projectiles!", "WARNING");
        return nullptr;
    }
    Projectile* p = m_availableProjectiles.front();
    m_availableProjectiles.pop();
    return p;
}

Projectile* ProjectilePool::GetProjectile(ProjectileType type)
{
    const size_t availableCount = m_availableProjectiles.size();
    for (size_t index = 0; index < availableCount; ++index)
    {
        Projectile* projectile = m_availableProjectiles.front();
        m_availableProjectiles.pop();

        const bool matches = (type == ProjectileType::BULLET && dynamic_cast<Bullet*>(projectile)) ||
                             (type == ProjectileType::ROCKET && dynamic_cast<Rocket*>(projectile)) ||
                             (type == ProjectileType::GRENADE && dynamic_cast<Grenade*>(projectile));
        if (matches)
            return projectile;

        m_availableProjectiles.push(projectile);
    }

    FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool: No projectile available for requested weapon type", "WARNING");
    return nullptr;
}

void ProjectilePool::ReturnProjectile(Projectile* p)
{
    // **FIXED: Rate-limited logging for projectile return**
    FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool::ReturnProjectile called.", "OPERATION");
    FPS_REQUIRE_NOT_NULL(p);
    if (p)
    {
        p->Deactivate();
        m_availableProjectiles.push(p);
    }
}

void ProjectilePool::FireBullet(const XMFLOAT3& pos, const XMFLOAT3& dir, float speed)
{
    FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool::FireBullet called. speed=" + std::to_string(speed), "OPERATION");
    FPS_REQUIRE_MSG(speed >= 0.0f, "Speed must be non-negative in FireBullet");
    if (auto p = GetProjectile(ProjectileType::BULLET))
    {
        p->SetDamage(15.0f);
        p->Fire(pos, dir, speed);
    }
}

void ProjectilePool::FireRocket(const XMFLOAT3& pos, const XMFLOAT3& dir, float speed)
{
    FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool::FireRocket called. speed=" + std::to_string(speed), "OPERATION");
    FPS_REQUIRE_MSG(speed >= 0.0f, "Speed must be non-negative in FireRocket");
    if (auto p = GetProjectile(ProjectileType::ROCKET))
    {
        p->SetDamage(75.0f);
        p->Fire(pos, dir, speed);
    }
}

void ProjectilePool::FireGrenade(const XMFLOAT3& pos, const XMFLOAT3& dir, float speed)
{
    FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool::FireGrenade called. speed=" + std::to_string(speed), "OPERATION");
    FPS_REQUIRE_MSG(speed >= 0.0f, "Speed must be non-negative in FireGrenade");
    if (auto p = GetProjectile(ProjectileType::GRENADE))
    {
        p->SetDamage(100.0f);
        p->SetGravity(true, 1.0f);
        p->Fire(pos, dir, speed);
    }
}

void ProjectilePool::FireProjectile(ProjectileType type, const XMFLOAT3& pos, const XMFLOAT3& dir, float speed,
                                    float damage)
{
    // **FIXED: Rate-limited logging for weapon firing**
    static auto lastFireLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastFireLog).count();

    Projectile* projectile = GetProjectile(type);
    if (!projectile)
        return;

    float defaultDamage = 15.0f;
    switch (type)
    {
    case ProjectileType::BULLET:
        defaultDamage = 15.0f;
        break;
    case ProjectileType::ROCKET:
        defaultDamage = 75.0f;
        break;
    case ProjectileType::GRENADE:
        defaultDamage = 100.0f;
        projectile->SetGravity(true, 1.0f);
        break;
    default:
        FPS_CONSOLE("Unknown ProjectileType in FireProjectile", "ERROR");
        ReturnProjectile(projectile);
        FPS_REQUIRE_MSG(false, "Unknown ProjectileType in FireProjectile");
        return;
    }

    projectile->SetDamage(damage >= 0.0f ? damage : defaultDamage);
    projectile->Fire(pos, dir, speed);

    // Only log firing every 3 seconds to avoid spam
    if (elapsed >= 3)
    {
        FPS_CONSOLE_RATE_LIMITED(1, 10, "ProjectilePool: Projectile fired.", "INFO");
        lastFireLog = now;
    }
}

size_t ProjectilePool::ResolveEnemyHits(const std::vector<Enemy*>& enemies, Spark::EventBus* eventBus)
{
    size_t hitCount = 0;

    for (auto& ownedProjectile : m_projectiles)
    {
        Projectile* projectile = ownedProjectile.get();
        if (!projectile->IsActive())
            continue;

        const XMFLOAT3 start = projectile->GetPreviousPosition();
        const XMFLOAT3 end = projectile->GetPosition();
        const float segmentX = end.x - start.x;
        const float segmentY = end.y - start.y;
        const float segmentZ = end.z - start.z;
        const float segmentLengthSq = segmentX * segmentX + segmentY * segmentY + segmentZ * segmentZ;

        for (Enemy* enemy : enemies)
        {
            if (!enemy || !enemy->IsAlive())
                continue;

            const XMFLOAT3 enemyPosition = enemy->GetPosition();
            float interpolation = 0.0f;
            if (segmentLengthSq > 0.000001f)
            {
                interpolation = ((enemyPosition.x - start.x) * segmentX + (enemyPosition.y - start.y) * segmentY +
                                 (enemyPosition.z - start.z) * segmentZ) /
                                segmentLengthSq;
                interpolation = std::clamp(interpolation, 0.0f, 1.0f);
            }

            const float closestX = start.x + segmentX * interpolation;
            const float closestY = start.y + segmentY * interpolation;
            const float closestZ = start.z + segmentZ * interpolation;
            const float dx = enemyPosition.x - closestX;
            const float dy = enemyPosition.y - closestY;
            const float dz = enemyPosition.z - closestZ;
            const auto& scale = enemy->GetScale();
            const float enemyRadius =
                (std::max)(0.75f, (std::max)({std::abs(scale.x), std::abs(scale.y), std::abs(scale.z)}));
            const float combinedRadius = enemyRadius + projectile->GetBoundingSphere().Radius;
            if (dx * dx + dy * dy + dz * dz > combinedRadius * combinedRadius)
                continue;

            enemy->TakeDamage(projectile->GetDamage());
            const bool killed = !enemy->IsAlive();
            const char* cause = dynamic_cast<Rocket*>(projectile)    ? "Rocket"
                                : dynamic_cast<Grenade*>(projectile) ? "Grenade"
                                                                     : "Bullet";
            projectile->OnHit(enemy);
            ReturnProjectile(projectile);
            ++hitCount;

            if (killed && eventBus)
            {
                eventBus->Publish(Spark::EntityKilledEvent{enemy->GetID(), 0, cause});
            }
            break;
        }
    }

    return hitCount;
}

void ProjectilePool::SetPhysicsSystem(PhysicsSystem* ps)
{
    m_physicsSystem = ps;
    for (auto& p : m_projectiles)
        p->SetPhysicsSystem(ps);
}

size_t ProjectilePool::GetActiveCount() const
{
    return static_cast<size_t>(
        std::count_if(m_projectiles.begin(), m_projectiles.end(), [](const auto& p) { return p->IsActive(); }));
}

size_t ProjectilePool::GetAvailableCount() const
{
    return m_availableProjectiles.size();
}
