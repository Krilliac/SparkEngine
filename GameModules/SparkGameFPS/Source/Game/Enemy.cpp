/**
 * @file Enemy.cpp
 * @brief AI-driven enemy implementation using engine BehaviorTree system
 */

#include "Core/Platform.h"
#ifdef SPARK_PLATFORM_WINDOWS
#include "Core/Platform.h"
#endif

#include "Enemy.h"
#include "Player.h"
#include "Utils/LogMacros.h"

#include <cmath>
#include <algorithm>

using namespace DirectX;

// Helper: squared distance between two XMFLOAT3 points
static float DistanceSquared(const XMFLOAT3& a, const XMFLOAT3& b)
{
    float dx = a.x - b.x;
    float dy = a.y - b.y;
    float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

static float Distance(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return std::sqrt(DistanceSquared(a, b));
}

// =============================================================================
// Lifecycle
// =============================================================================

Enemy::Enemy() = default;

Enemy::~Enemy() = default;

HRESULT Enemy::Initialize(ID3D11Device* device, ID3D11DeviceContext* context, EnemyType type, Player* player)
{
    HRESULT hr = GameObject::Initialize(device, context);
    if (FAILED(hr))
        return hr;

    m_type = type;
    m_player = player;

    // Configure stats based on archetype
    switch (type)
    {
    case EnemyType::Grunt:
        m_health = m_maxHealth = 100.0f;
        m_damage = 10.0f;
        m_moveSpeed = 5.0f;
        m_attackCooldown = 1.0f;
        m_aiConfig.detectionRange = 25.0f;
        m_aiConfig.attackRange = 12.0f;
        m_aiConfig.moveSpeed = 5.0f;
        m_aiConfig.accuracy = 0.6f;
        m_aiConfig.reactionTime = 0.4f;
        break;

    case EnemyType::Guard:
        m_health = m_maxHealth = 150.0f;
        m_damage = 12.0f;
        m_moveSpeed = 3.0f;
        m_attackCooldown = 1.2f;
        m_aiConfig.detectionRange = 35.0f;
        m_aiConfig.attackRange = 18.0f;
        m_aiConfig.moveSpeed = 3.0f;
        m_aiConfig.accuracy = 0.75f;
        m_aiConfig.reactionTime = 0.3f;
        break;

    case EnemyType::Scout:
        m_health = m_maxHealth = 60.0f;
        m_damage = 8.0f;
        m_moveSpeed = 8.0f;
        m_attackCooldown = 0.6f;
        m_aiConfig.detectionRange = 30.0f;
        m_aiConfig.attackRange = 10.0f;
        m_aiConfig.moveSpeed = 8.0f;
        m_aiConfig.accuracy = 0.5f;
        m_aiConfig.reactionTime = 0.2f;
        m_aiConfig.canStrafe = true;
        break;

    case EnemyType::Heavy:
        m_health = m_maxHealth = 300.0f;
        m_damage = 20.0f;
        m_moveSpeed = 2.5f;
        m_attackCooldown = 2.0f;
        m_aiConfig.detectionRange = 20.0f;
        m_aiConfig.attackRange = 8.0f;
        m_aiConfig.moveSpeed = 2.5f;
        m_aiConfig.accuracy = 0.8f;
        m_aiConfig.reactionTime = 0.5f;
        m_aiConfig.canSprint = false;
        break;

    case EnemyType::Sniper:
        m_health = m_maxHealth = 70.0f;
        m_damage = 30.0f;
        m_moveSpeed = 3.5f;
        m_attackCooldown = 2.5f;
        m_aiConfig.detectionRange = 50.0f;
        m_aiConfig.attackRange = 40.0f;
        m_aiConfig.moveSpeed = 3.5f;
        m_aiConfig.accuracy = 0.9f;
        m_aiConfig.reactionTime = 0.6f;
        m_aiConfig.canStrafe = true;
        break;

    case EnemyType::Medic:
        m_health = m_maxHealth = 120.0f;
        m_damage = 8.0f;
        m_moveSpeed = 5.5f;
        m_attackCooldown = 1.5f;
        m_aiConfig.detectionRange = 30.0f;
        m_aiConfig.attackRange = 14.0f;
        m_aiConfig.moveSpeed = 5.5f;
        m_aiConfig.accuracy = 0.55f;
        m_aiConfig.reactionTime = 0.3f;
        m_healRange = 15.0f;
        m_healAmount = 5.0f;
        m_healCooldown = 2.0f;
        break;
    }

    m_turnSpeed = m_aiConfig.turnSpeed;
    m_moveSpeed = m_aiConfig.moveSpeed;

    SPARK_LOG_INFO(Spark::LogCategory::Game, "Enemy spawned: type=%d, HP=%.0f, speed=%.1f", static_cast<int>(type),
                   m_health, m_moveSpeed);

    BuildBehaviorTree();

    SetScale({1.0f, 2.0f, 1.0f}); // Tall capsule-like shape

    return S_OK;
}

// =============================================================================
// Behavior Tree Construction
// =============================================================================

void Enemy::BuildBehaviorTree()
{
    using namespace Spark::AI;

    // Root selector: combat if enemy visible, else patrol/idle
    auto root = std::make_unique<SelectorNode>("Root");

    // Branch 1: Combat — if enemy is visible and in range, attack
    auto combatSeq = std::make_unique<SequenceNode>("CombatSequence");

    combatSeq->AddChild(std::make_unique<ConditionNode>("EnemyVisible", [](const Blackboard& bb)
                                                        { return bb.Get<bool>("enemyVisible", false); }));

    combatSeq->AddChild(std::make_unique<ActionNode>("Engage",
                                                     [this](float dt, Blackboard& bb) -> NodeStatus
                                                     {
                                                         float dist = bb.Get<float>("distToTarget", 999.0f);
                                                         XMFLOAT3 targetPos = bb.Get<XMFLOAT3>("targetPos");

                                                         FaceTarget(targetPos, dt);

                                                         // Sniper: retreat if player gets too close
                                                         if (m_type == EnemyType::Sniper &&
                                                             dist < m_aiConfig.attackRange * 0.4f)
                                                         {
                                                             MoveAwayFrom(targetPos, dt);
                                                             return NodeStatus::Running;
                                                         }

                                                         if (dist > m_aiConfig.attackRange)
                                                         {
                                                             MoveToward(targetPos, dt);
                                                             return NodeStatus::Running;
                                                         }

                                                         // Medic: alternate between healing and attacking
                                                         if (m_type == EnemyType::Medic)
                                                             HealNearby(dt);

                                                         Attack(dt);
                                                         return NodeStatus::Running;
                                                     }));

    root->AddChild(std::move(combatSeq));

    // Branch 2: Patrol waypoints (if any)
    if (!m_patrolPoints.empty())
    {
        root->AddChild(std::make_unique<ActionNode>("Patrol",
                                                    [this](float dt, Blackboard&) -> NodeStatus
                                                    {
                                                        if (m_patrolPoints.empty())
                                                            return NodeStatus::Failure;

                                                        const XMFLOAT3& target = m_patrolPoints[m_currentPatrolIndex];
                                                        float dist = Distance(GetPosition(), target);

                                                        if (dist < 1.5f)
                                                        {
                                                            m_currentPatrolIndex =
                                                                (m_currentPatrolIndex + 1) % m_patrolPoints.size();
                                                            return NodeStatus::Success;
                                                        }

                                                        FaceTarget(target, dt);
                                                        MoveToward(target, dt);
                                                        return NodeStatus::Running;
                                                    }));
    }
    else
    {
        // Idle — do nothing
        root->AddChild(
            std::make_unique<ActionNode>("Idle", [](float, Blackboard&) -> NodeStatus { return NodeStatus::Running; }));
    }

    m_behaviorTree = std::make_unique<BehaviorTree>("Enemy_" + std::to_string(GetID()));
    m_behaviorTree->SetRoot(std::move(root));
}

// =============================================================================
// Per-Frame Update
// =============================================================================

void Enemy::Update(float dt)
{
    if (!IsActive() || !IsAlive())
        return;

    UpdatePerception(dt);

    if (m_behaviorTree)
        m_behaviorTree->Tick(dt);

    // Decrement timers
    if (m_attackTimer > 0.0f)
        m_attackTimer -= dt;
    if (m_healTimer > 0.0f)
        m_healTimer -= dt;

    // Apply velocity
    XMFLOAT3 pos = GetPosition();
    pos.x += m_velocity.x * dt;
    pos.y += m_velocity.y * dt;
    pos.z += m_velocity.z * dt;
    SetPosition(pos);

    // Dampen velocity
    m_velocity.x *= 0.9f;
    m_velocity.z *= 0.9f;
}

void Enemy::Render(const XMMATRIX& view, const XMMATRIX& proj)
{
    if (!IsVisible() || !IsAlive())
        return;

    GameObject::Render(view, proj);
}

// =============================================================================
// Perception — writes into the Blackboard each frame
// =============================================================================

void Enemy::UpdatePerception(float dt)
{
    (void)dt;
    if (!m_behaviorTree || !m_player || !m_player->IsAlive())
    {
        if (m_behaviorTree)
        {
            auto& bb = m_behaviorTree->GetBlackboard();
            bb.Set("enemyVisible", false);
        }
        return;
    }

    auto& bb = m_behaviorTree->GetBlackboard();
    XMFLOAT3 playerPos = m_player->GetPosition();
    float dist = Distance(GetPosition(), playerPos);

    bool visible = dist <= m_aiConfig.detectionRange;
    bb.Set("enemyVisible", visible);
    bb.Set("targetPos", playerPos);
    bb.Set("distToTarget", dist);
}

// =============================================================================
// Movement Helpers
// =============================================================================

void Enemy::MoveToward(const XMFLOAT3& target, float dt)
{
    XMFLOAT3 pos = GetPosition();
    float dx = target.x - pos.x;
    float dz = target.z - pos.z;
    float len = std::sqrt(dx * dx + dz * dz);

    if (len > 0.01f)
    {
        float invLen = 1.0f / len;
        m_velocity.x = dx * invLen * m_moveSpeed;
        m_velocity.z = dz * invLen * m_moveSpeed;
    }

    (void)dt;
}

void Enemy::MoveAwayFrom(const XMFLOAT3& target, float dt)
{
    XMFLOAT3 pos = GetPosition();
    float dx = pos.x - target.x;
    float dz = pos.z - target.z;
    float len = std::sqrt(dx * dx + dz * dz);

    if (len > 0.01f)
    {
        float invLen = 1.0f / len;
        m_velocity.x = dx * invLen * m_moveSpeed;
        m_velocity.z = dz * invLen * m_moveSpeed;
    }
    (void)dt;
}

void Enemy::HealNearby(float dt)
{
    (void)dt;
    if (m_healTimer > 0.0f)
        return;

    // Medic heals are a no-op in single-player without an ally list,
    // but the timer resets so the medic alternates between attacking and "healing"
    m_healTimer = m_healCooldown;
}

void Enemy::FaceTarget(const XMFLOAT3& target, float dt)
{
    XMFLOAT3 pos = GetPosition();
    float dx = target.x - pos.x;
    float dz = target.z - pos.z;
    float targetYaw = std::atan2(dx, dz);

    XMFLOAT3 rot = GetRotation();
    float diff = targetYaw - rot.y;

    // Normalize to [-PI, PI]
    while (diff > XM_PI)
        diff -= XM_2PI;
    while (diff < -XM_PI)
        diff += XM_2PI;

    float maxTurn = XMConvertToRadians(m_turnSpeed) * dt;
    if (std::abs(diff) > maxTurn)
        diff = (diff > 0.0f) ? maxTurn : -maxTurn;

    rot.y += diff;
    SetRotation(rot);
}

// =============================================================================
// Combat
// =============================================================================

void Enemy::Attack(float dt)
{
    (void)dt;
    if (m_attackTimer > 0.0f || !m_player || !m_player->IsAlive())
        return;

    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Enemy attacking player for %.0f damage", m_damage);
    m_player->TakeDamage(m_damage);
    m_attackTimer = m_attackCooldown;
}

void Enemy::TakeDamage(float dmg)
{
    if (!IsAlive())
        return;

    m_health = std::max(0.0f, m_health - dmg);

    if (m_health <= 0.0f)
    {
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Enemy killed: type=%d, ID=%u", static_cast<int>(m_type), GetID());
        SetActive(false);
        SetVisible(false);
    }
}

void Enemy::OnHit(GameObject* target)
{
    (void)target;
}

void Enemy::OnHitWorld(const XMFLOAT3& hitPoint, const XMFLOAT3& normal)
{
    (void)hitPoint;
    (void)normal;
}

// =============================================================================
// Configuration
// =============================================================================

void Enemy::SetPatrolPoints(const std::vector<XMFLOAT3>& points)
{
    SPARK_LOG_DEBUG(Spark::LogCategory::Game, "Enemy patrol route set: %zu waypoints", points.size());
    m_patrolPoints = points;
    m_currentPatrolIndex = 0;
    BuildBehaviorTree(); // Rebuild to include patrol branch
}
