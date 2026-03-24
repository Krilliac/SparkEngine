/**
 * @file Enemy.h
 * @brief AI-driven enemy entity for the SparkGame arena
 * @author Spark Engine Team
 * @date 2025
 *
 * Wraps the engine's BehaviorTree and Blackboard systems into a
 * self-contained GameObject that patrols, detects the player, and attacks.
 *
 * @see BehaviorTree.h, BehaviorTreeNodes.h, BehaviorTreeTypes.h
 */

#pragma once

#include "Spark/SparkExport.h"
#include "Core/Platform.h"
#include "Game/GameObject.h"
#include "Engine/AI/BehaviorTree.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include <DirectXMath.h>
#endif

#include <memory>
#include <string>
#include <vector>

class Player;

/**
 * @brief Enemy archetype presets
 */
enum class EnemyType
{
    Grunt,  ///< Basic patrol + attack
    Guard,  ///< Stationary sentry
    Scout,  ///< Fast, low-health flanker
    Heavy,  ///< Slow, high-health tank
    Sniper, ///< Long-range, high damage, low health
    Medic   ///< Heals nearby allies, moderate combat
};

/**
 * @brief AI-driven enemy that uses a BehaviorTree for decisions
 *
 * Each Enemy owns its own BehaviorTree instance and Blackboard.
 * The Game class updates perception data (player position, visibility)
 * each frame, then the tree ticks to produce movement and combat actions.
 */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4251)
#pragma warning(disable : 4275)
#endif

class SPARK_GAME_API Enemy : public GameObject
{
  public:
    using GameObject::Initialize;

    Enemy();
    ~Enemy() override;

    /**
     * @brief Initialize with D3D resources and enemy configuration
     * @param device D3D11 device
     * @param context D3D11 device context
     * @param type Enemy archetype
     * @param player Pointer to the player (for perception)
     * @return HRESULT
     */
    HRESULT Initialize(ID3D11Device* device, ID3D11DeviceContext* context, EnemyType type, Player* player);

    void Update(float dt) override;
    void Render(const DirectX::XMMATRIX& view, const DirectX::XMMATRIX& proj) override;

    void OnHit(GameObject* target) override;
    void OnHitWorld(const DirectX::XMFLOAT3& hitPoint, const DirectX::XMFLOAT3& normal) override;

    /** @brief Apply damage to this enemy */
    void TakeDamage(float dmg);

    /** @brief Check if still alive */
    bool IsAlive() const { return m_health > 0.0f; }

    /** @brief Get current health */
    float GetHealth() const { return m_health; }

    /** @brief Get max health */
    float GetMaxHealth() const { return m_maxHealth; }

    /** @brief Get the enemy type */
    EnemyType GetType() const { return m_type; }

    /** @brief Set patrol waypoints (replaces current behavior with patrol+combat) */
    void SetPatrolPoints(const std::vector<DirectX::XMFLOAT3>& points);

  private:
    void BuildBehaviorTree();
    void UpdatePerception(float dt);
    void MoveToward(const DirectX::XMFLOAT3& target, float dt);
    void MoveAwayFrom(const DirectX::XMFLOAT3& target, float dt);
    void FaceTarget(const DirectX::XMFLOAT3& target, float dt);
    void Attack(float dt);
    void HealNearby(float dt);

    // AI
    std::unique_ptr<Spark::AI::BehaviorTree> m_behaviorTree;
    Spark::AI::AIAgentConfig m_aiConfig;
    EnemyType m_type{EnemyType::Grunt};
    Player* m_player{nullptr};
    std::vector<DirectX::XMFLOAT3> m_patrolPoints;
    size_t m_currentPatrolIndex{0};

    // Stats
    float m_health{100.0f};
    float m_maxHealth{100.0f};
    float m_damage{10.0f};
    float m_attackCooldown{1.0f};
    float m_attackTimer{0.0f};

    // Movement
    DirectX::XMFLOAT3 m_velocity{};
    float m_moveSpeed{5.0f};
    float m_turnSpeed{180.0f};

    // Medic-specific
    float m_healRange{15.0f};
    float m_healAmount{5.0f};
    float m_healCooldown{2.0f};
    float m_healTimer{0.0f};
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif
