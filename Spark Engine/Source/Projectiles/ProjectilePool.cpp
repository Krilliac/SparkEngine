#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
// ProjectilePool.cpp
#include "ProjectilePool.h"
#include "Bullet.h"
#include "Rocket.h"
#include "Grenade.h"
#include "Utils/Assert.h"
#include "../Utils/ConsoleProcessManager.h"
#include <iostream>
#include <memory>

using namespace DirectX;

// **FIXED: Rate-limited logging for ProjectilePool to prevent console spam**
#define LOG_TO_CONSOLE_RATE_LIMITED(msg, type) \
    do { \
        static auto lastLogTime = std::chrono::steady_clock::now(); \
        static int logCounter = 0; \
        auto now = std::chrono::steady_clock::now(); \
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastLogTime).count(); \
        if (elapsed >= 10 || logCounter < 1) { \
            Spark::ConsoleProcessManager::GetInstance().Log(msg, type); \
            if (elapsed >= 10) { \
                lastLogTime = now; \
                logCounter = 0; \
            } else { \
                logCounter++; \
            } \
        } \
    } while(0)

// Use rate-limited logging for most messages, immediate for critical ones
#define LOG_TO_CONSOLE(msg, type) LOG_TO_CONSOLE_RATE_LIMITED(msg, type)
#define LOG_TO_CONSOLE_IMMEDIATE(msg, type) Spark::ConsoleProcessManager::GetInstance().Log(msg, type)

ProjectilePool::ProjectilePool(size_t poolSize)
    : m_poolSize(poolSize)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"ProjectilePool constructed with size " + std::to_wstring(poolSize), L"INFO");
    ASSERT_MSG(poolSize > 0, "ProjectilePool size must be positive");
    m_projectiles.reserve(poolSize);
}

ProjectilePool::~ProjectilePool()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"ProjectilePool destructor called.", L"INFO");
    Shutdown();
}

HRESULT ProjectilePool::Initialize(ID3D11Device* device, ID3D11DeviceContext* context)
{
    LOG_TO_CONSOLE_IMMEDIATE(L"ProjectilePool::Initialize called.", L"OPERATION");
    ASSERT(device != nullptr);
    ASSERT(context != nullptr);

    m_device = device;
    m_context = context;

    // Create projectiles based on pool size distribution
    size_t bulletsCount = m_poolSize / 2;     // 50% bullets
    size_t rocketsCount = m_poolSize / 4;     // 25% rockets
    size_t grenadesCount = m_poolSize - bulletsCount - rocketsCount; // remainder

    // Helper lambda
    auto makeAndStore = [&](auto TypeFactory, size_t count) {
        for (size_t i = 0; i < count; ++i)
        {
            auto p = TypeFactory();
            ASSERT_MSG(p != nullptr, "Failed to create projectile");
            if (SUCCEEDED(p->Initialize(m_device, m_context)))
            {
                m_availableProjectiles.push(p.get());
                m_projectiles.push_back(std::move(p));
            }
        }
        };

    makeAndStore([] { return std::make_unique<Bullet>(); }, bulletsCount);
    makeAndStore([] { return std::make_unique<Rocket>(); }, rocketsCount);
    makeAndStore([] { return std::make_unique<Grenade>(); }, grenadesCount);

    ASSERT_MSG(m_projectiles.size() == m_poolSize, "Some projectiles failed to initialize");

    LOG_TO_CONSOLE_IMMEDIATE(L"ProjectilePool created " + std::to_wstring(m_projectiles.size()) + L" projectiles.", L"INFO");
    return S_OK;
}

void ProjectilePool::Update(float deltaTime)
{
    // **FIXED: Remove per-frame logging completely**
    ASSERT_MSG(deltaTime >= 0.0f && std::isfinite(deltaTime), "Invalid deltaTime in ProjectilePool::Update");

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
    for (auto& up : m_projectiles)
    {
        if (up->IsActive())
            up->Render(view, projection);
    }
}

void ProjectilePool::Shutdown()
{
    LOG_TO_CONSOLE_IMMEDIATE(L"ProjectilePool::Shutdown called.", L"OPERATION");

    m_projectiles.clear();
    std::queue<Projectile*> empty;
    std::swap(m_availableProjectiles, empty);

    LOG_TO_CONSOLE_IMMEDIATE(L"ProjectilePool shutdown complete.", L"INFO");
}

Projectile* ProjectilePool::GetProjectile()
{
    // **FIXED: Rate-limited logging for projectile acquisition**
    LOG_TO_CONSOLE(L"ProjectilePool::GetProjectile called.", L"OPERATION");
    if (m_availableProjectiles.empty()) {
        LOG_TO_CONSOLE(L"ProjectilePool: No available projectiles!", L"WARNING");
        return nullptr;
    }
    Projectile* p = m_availableProjectiles.front();
    m_availableProjectiles.pop();
    return p;
}

void ProjectilePool::ReturnProjectile(Projectile* p)
{
    // **FIXED: Rate-limited logging for projectile return**
    LOG_TO_CONSOLE(L"ProjectilePool::ReturnProjectile called.", L"OPERATION");
    ASSERT(p != nullptr);
    if (p)
    {
        p->SetActive(false);
        m_availableProjectiles.push(p);
    }
}

void ProjectilePool::FireBullet(const XMFLOAT3& pos, const XMFLOAT3& dir, float speed)
{
    LOG_TO_CONSOLE(L"ProjectilePool::FireBullet called. speed=" + std::to_wstring(speed), L"OPERATION");
    ASSERT_MSG(speed >= 0.0f, "Speed must be non-negative in FireBullet");
    if (auto p = GetProjectile()) {
        p->Fire(pos, dir, speed);
    }
}

void ProjectilePool::FireRocket(const XMFLOAT3& pos, const XMFLOAT3& dir, float speed)
{
    LOG_TO_CONSOLE(L"ProjectilePool::FireRocket called. speed=" + std::to_wstring(speed), L"OPERATION");
    ASSERT_MSG(speed >= 0.0f, "Speed must be non-negative in FireRocket");
    if (auto p = GetProjectile()) {
        p->Fire(pos, dir, speed);
    }
}

void ProjectilePool::FireGrenade(const XMFLOAT3& pos, const XMFLOAT3& dir, float speed)
{
    LOG_TO_CONSOLE(L"ProjectilePool::FireGrenade called. speed=" + std::to_wstring(speed), L"OPERATION");
    ASSERT_MSG(speed >= 0.0f, "Speed must be non-negative in FireGrenade");
    if (auto p = GetProjectile()) {
        p->SetGravity(true, 1.0f);
        p->Fire(pos, dir, speed);
    }
}

void ProjectilePool::FireProjectile(ProjectileType type, const XMFLOAT3& pos, const XMFLOAT3& dir, float speed)
{
    // **FIXED: Rate-limited logging for weapon firing**
    static auto lastFireLog = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - lastFireLog).count();
    
    switch (type)
    {
    case ProjectileType::BULLET:  FireBullet(pos, dir, speed); break;
    case ProjectileType::ROCKET:  FireRocket(pos, dir, speed); break;
    case ProjectileType::GRENADE: FireGrenade(pos, dir, speed); break;
    default:
        LOG_TO_CONSOLE_IMMEDIATE(L"Unknown ProjectileType in FireProjectile", L"ERROR");
        ASSERT_MSG(false, "Unknown ProjectileType in FireProjectile");
    }
    
    // Only log firing every 3 seconds to avoid spam
    if (elapsed >= 3) {
        LOG_TO_CONSOLE(L"ProjectilePool: Projectile fired.", L"INFO");
        lastFireLog = now;
    }
}

size_t ProjectilePool::GetActiveCount() const
{
    // **FIXED: Rate-limited logging for count queries**
    LOG_TO_CONSOLE(L"ProjectilePool::GetActiveCount called.", L"OPERATION");
    size_t count = 0;
    for (const auto& up : m_projectiles)
        if (up->IsActive()) ++count;
    return count;
}

size_t ProjectilePool::GetAvailableCount() const
{
    // **FIXED: Rate-limited logging for count queries**
    LOG_TO_CONSOLE(L"ProjectilePool::GetAvailableCount called.", L"OPERATION");
    return m_availableProjectiles.size();
}
#endif // SPARK_PLATFORM_WINDOWS
