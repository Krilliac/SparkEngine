/**
 * @file AbilitySystem.h
 * @brief Pipeline-based ability/spell system with auras and procs (TrinityCore-inspired)
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a generalized spell/ability/aura/proc system modeled after TrinityCore's
 * spell pipeline. Abilities flow through a cast pipeline (Prepare -> Cast -> Launch),
 * can apply persistent auras (buffs/debuffs/DoTs/HoTs), and trigger proc chains
 * when specific combat events occur.
 *
 * ## Concepts (mapped to TrinityCore equivalents)
 * - AbilityDefinition  = SpellInfo   (static data describing what an ability does)
 * - AbilityCastInstance = Spell       (active cast in flight)
 * - AuraDefinition      = AuraInfo   (static data for a persistent effect)
 * - ActiveAura          = Aura       (runtime instance on an entity)
 * - ProcDefinition      = spell_proc (triggered effect rules)
 *
 * ## Pipeline
 * CastAbility() -> Prepare -> Casting (cast bar) -> Launch (resolve effects) -> Hit/Proc
 *
 * ## Integration
 * - Uses HealthComponent for damage/heal application
 * - Publishes EntityDamagedEvent / EntityKilledEvent through EventBus
 * - Call Update() once per frame from the gameplay phase
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// Forward declarations — avoid pulling in heavy ECS/event headers
class World;

namespace Spark
{
    class EventBus;
}

namespace Spark::Gameplay
{

    // ============================================================================
    // ID Types
    // ============================================================================

    using AbilityID = uint32_t;
    using AuraID = uint32_t;

    // ============================================================================
    // Ability Enums
    // ============================================================================

    enum class AbilityTargetType : uint8_t
    {
        Self,
        SingleTarget,
        AreaOfEffect,
        Cone,
        Projectile
    };

    enum class AbilitySchool : uint8_t
    {
        Physical = 0,
        Fire,
        Ice,
        Lightning,
        Nature,
        Shadow,
        Holy,
        Arcane,
        Count
    };

    enum class EffectType : uint8_t
    {
        Damage,
        Heal,
        ApplyAura,
        RemoveAura,
        Summon,
        Teleport,
        ModifyAttribute,
        ApplyForce,
        SpawnProjectile,
        Custom
    };

    // ============================================================================
    // Ability Effect (one of up to 3 effects per ability, like TC's SpellEffectInfo)
    // ============================================================================

    struct AbilityEffect
    {
        EffectType type = EffectType::Damage;           ///< What this effect does (damage, heal, apply aura, etc.).
        float baseValue = 0.0f;                         ///< Base amount (damage, healing, attribute change).
        float scaling = 1.0f;                           ///< Multiplier for attribute scaling.
        AbilitySchool school = AbilitySchool::Physical; ///< Damage school (for resistance calculations).
        uint32_t auraId = 0;                            ///< Aura ID for ApplyAura/RemoveAura effects.
        uint32_t customParam = 0;                       ///< User-defined parameter for Custom effects.
    };

    // ============================================================================
    // Ability Definition (static data — like TC's SpellInfo)
    // ============================================================================

    struct AbilityDefinition
    {
        AbilityID id = 0;        ///< Unique ability identifier.
        std::string name;        ///< Display name (e.g. "Fireball", "Heal").
        std::string description; ///< Tooltip description.

        AbilityTargetType targetType = AbilityTargetType::SingleTarget; ///< Targeting mode.
        float range = 10.0f;                                            ///< Maximum cast range (meters).
        float radius = 0.0f;       ///< Effect radius for AoE abilities (0 = single target).
        float castTime = 0.0f;     ///< Cast bar duration (0 = instant cast).
        float cooldown = 1.0f;     ///< Seconds before the ability can be used again.
        float resourceCost = 0.0f; ///< Mana/stamina/energy cost to cast.

        std::vector<AbilityEffect> effects; ///< Up to 3 effects per ability (like TC).

        bool requiresTarget = true;      ///< Whether a valid target must be selected.
        bool canCastWhileMoving = false; ///< Whether the caster can move during the cast.
        bool isChanneled = false;        ///< Whether this is a channeled spell.
        float channelDuration = 0.0f;    ///< Duration of the channel (seconds).

        uint32_t iconIndex = 0;   ///< UI icon index for the ability bar.
        uint32_t animationId = 0; ///< Cast animation to play.
        uint32_t soundId = 0;     ///< Sound effect to play on cast.
    };

    // ============================================================================
    // Aura Enums
    // ============================================================================

    enum class AuraType : uint8_t
    {
        Buff,
        Debuff,
        DamageOverTime,
        HealOverTime,
        Shield,
        ModifySpeed,
        ModifyDamageDealt,
        ModifyDamageTaken,
        Stun,
        Root,
        Silence,
        Stealth,
        Taunt,
        Custom
    };

    enum class AuraStackType : uint8_t
    {
        None,     ///< Does not stack — refreshes duration
        Stacking, ///< Stacks up to maxStacks
        Separate  ///< Each application is independent
    };

    // ============================================================================
    // Aura Definition (static data — like TC's AuraInfo)
    // ============================================================================

    struct AuraDefinition
    {
        AuraID id = 0;
        std::string name;
        AuraType type = AuraType::Buff;
        AbilitySchool school = AbilitySchool::Physical;

        float duration = 10.0f;       ///< 0 = permanent until removed
        float tickInterval = 1.0f;    ///< For DoT/HoT
        float valuePerTick = 0.0f;    ///< Damage/heal per tick
        float flatModifier = 0.0f;    ///< Flat stat modification
        float percentModifier = 0.0f; ///< Percentage modification (1.0 = +100%)

        AuraStackType stackType = AuraStackType::None;
        int maxStacks = 1;

        bool isPermanent = false;
        bool isHidden = false; ///< Don't show in UI
        bool dispellable = true;
    };

    // ============================================================================
    // Proc Definition (triggered effects — like TC's spell_proc)
    // ============================================================================

    enum class ProcTrigger : uint32_t
    {
        None = 0,
        OnDealDamage = 1 << 0,
        OnTakeDamage = 1 << 1,
        OnHeal = 1 << 2,
        OnKill = 1 << 3,
        OnDeath = 1 << 4,
        OnAbilityCast = 1 << 5,
        OnCriticalHit = 1 << 6,
        OnDodge = 1 << 7,
        OnBlock = 1 << 8,
        OnHit = 1 << 9,
        OnMiss = 1 << 10,
    };

    struct ProcDefinition
    {
        AuraID sourceAuraId = 0;          ///< Which aura triggers this proc
        AbilityID triggeredAbilityId = 0; ///< What ability to cast when proc fires
        uint32_t triggerMask = 0;         ///< Bitmask of ProcTrigger values
        float chance = 100.0f;            ///< Proc chance (0-100)
        float cooldown = 0.0f;            ///< Internal cooldown in seconds
        int charges = 0;                  ///< 0 = unlimited; >0 = removes aura after N procs
        uint32_t schoolMask = 0xFF;       ///< Which schools can trigger (bitmask)
    };

    // ============================================================================
    // Active Aura Instance (runtime state on an entity)
    // ============================================================================

    struct ActiveAura
    {
        AuraID definitionId = 0;
        uint32_t casterId = 0;
        float remainingDuration = 0.0f;
        float tickTimer = 0.0f;
        int currentStacks = 1;
        int procChargesRemaining = 0;
        float procCooldownTimer = 0.0f;
        bool markedForRemoval = false;
    };

    // ============================================================================
    // Ability Cast Instance (active cast — like TC's Spell class)
    // ============================================================================

    enum class CastPhase : uint8_t
    {
        Preparing,  ///< Pre-cast validation passed, cast bar starting
        Casting,    ///< Cast time in progress
        Launching,  ///< Effects being dispatched
        Channeling, ///< Channeled ability in progress
        Completed,
        Failed,
        Interrupted
    };

    struct AbilityCastInstance
    {
        AbilityID abilityId = 0;
        uint32_t casterId = 0;
        uint32_t targetId = 0;     ///< For single-target abilities
        float castProgress = 0.0f; ///< 0.0 to castTime
        float channelRemaining = 0.0f;
        CastPhase phase = CastPhase::Preparing;
    };

    // ============================================================================
    // AbilitySystem — Registry + Pipeline
    // ============================================================================

    /**
     * @brief Manages ability definitions, aura definitions, proc tables, and the cast pipeline.
     *
     * TrinityCore-inspired design:
     * - AbilityDefinition = SpellInfo  (static data)
     * - AbilityCastInstance = Spell    (active cast)
     * - AuraDefinition + ActiveAura = Aura/AuraEffect (persistent effects)
     * - ProcDefinition = spell_proc   (triggered effects)
     *
     * Pipeline: Prepare -> Cast -> Launch (resolve effects) -> Hit/Proc
     *
     * @note Singleton. Call Initialize() at engine startup, Update() each frame.
     */
    class AbilitySystem
    {
      public:
        [[deprecated("Use EngineContext::Get()->GetSystem<AbilitySystem>() instead")]]
        static AbilitySystem& GetInstance();

        /**
         * @brief Initialize the ability system with an event bus for publishing combat events.
         * @param eventBus Pointer to the EventBus used for damage/kill/heal events.
         */
        void Initialize(EventBus* eventBus);
        void Shutdown();

        // -- Registration (static data) --
        void RegisterAbility(const AbilityDefinition& def);
        void RegisterAura(const AuraDefinition& def);
        void RegisterProc(const ProcDefinition& def);

        [[nodiscard]] const AbilityDefinition* GetAbilityDef(AbilityID id) const;
        [[nodiscard]] const AuraDefinition* GetAuraDef(AuraID id) const;

        // -- Cast Pipeline --
        bool CastAbility(World& world, uint32_t caster, AbilityID abilityId, uint32_t target = 0);
        void InterruptCast(uint32_t caster);
        [[nodiscard]] bool IsCasting(uint32_t caster) const;

        // -- Aura Management --
        void ApplyAura(World& world, uint32_t target, AuraID auraId, uint32_t caster);
        void RemoveAura(uint32_t target, AuraID auraId);
        void RemoveAllAuras(uint32_t target);
        [[nodiscard]] std::vector<ActiveAura> GetActiveAuras(uint32_t target) const;
        [[nodiscard]] bool HasAura(uint32_t target, AuraID auraId) const;
        [[nodiscard]] int GetAuraStacks(uint32_t target, AuraID auraId) const;

        // -- Frame Update --
        void Update(World& world, float deltaTime);

        // -- Stats --
        [[nodiscard]] int GetRegisteredAbilityCount() const { return static_cast<int>(m_abilities.size()); }
        [[nodiscard]] int GetRegisteredAuraCount() const { return static_cast<int>(m_auras.size()); }

      private:
        AbilitySystem() = default;

        void UpdateCasts(World& world, float dt);
        void UpdateAuras(World& world, float dt);
        void ProcessEffects(World& world, const AbilityDefinition& def, uint32_t caster, uint32_t target);
        void ProcessProcs(World& world, uint32_t triggerMask, uint32_t source, uint32_t target, AbilitySchool school);
        void ApplyEffect(World& world, const AbilityEffect& effect, uint32_t caster, uint32_t target);
        void TickAura(World& world, uint32_t target, ActiveAura& aura, const AuraDefinition& def);

        // Static data registries
        std::unordered_map<AbilityID, AbilityDefinition> m_abilities;
        std::unordered_map<AuraID, AuraDefinition> m_auras;
        std::vector<ProcDefinition> m_procs;

        // Per-entity runtime state
        std::unordered_map<uint32_t, AbilityCastInstance> m_activeCasts;
        std::unordered_map<uint32_t, std::vector<ActiveAura>> m_activeAuras;

        // Cooldown tracking: entity -> (abilityId -> remaining cooldown)
        std::unordered_map<uint32_t, std::unordered_map<AbilityID, float>> m_cooldowns;

        EventBus* m_eventBus = nullptr;
    };

} // namespace Spark::Gameplay
