/**
 * @file ScriptHookManager.h
 * @brief Centralized script hook dispatcher for gameplay events (TC ScriptMgr-inspired)
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a priority-based hook dispatch system that allows scripts to register
 * handlers for gameplay events (combat, entity lifecycle, quests, movement, etc.).
 * When the engine fires a hook, all registered handlers run in priority order and
 * can inspect or modify the event context (e.g. cancel an ability, modify damage).
 *
 * Designed after TrinityCore's ScriptMgr pattern: the engine dispatches hooks at
 * well-defined points, and any number of scripts can respond without the engine
 * needing to know about specific script logic.
 *
 * ## Usage
 * @code
 *   // Script registers a damage modifier
 *   auto& hooks = Spark::Scripting::ScriptHookManager::GetInstance();
 *   hooks.RegisterHook(HookType::OnDamageDealt, "ArmorScript",
 *       [](HookContext& ctx) {
 *           ctx.modifiedValue = ctx.floatParam1 * 0.8f;  // 20% reduction
 *           ctx.valueModified = true;
 *       }, 10);
 *
 *   // Engine dispatches when damage occurs
 *   auto result = hooks.DispatchCombatHook(HookType::OnDamageDealt, attackerId, targetId, rawDamage, school);
 *   float finalDamage = result.valueModified ? result.modifiedValue : rawDamage;
 * @endcode
 *
 * ## Thread safety
 * All public methods are mutex-protected. DispatchHook copies the handler list
 * under lock, then invokes handlers without holding the lock to avoid deadlocks
 * if a handler calls back into the manager.
 *
 * @see AngelScriptEngine.h for the scripting VM that compiles and runs scripts
 * @see EventSystem.h for the engine's internal event bus (separate from script hooks)
 */

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Spark::Scripting
{

    using EntityID = uint32_t;

    // =============================================================================
    // Hook Categories
    // =============================================================================

    /**
 * @brief Hook categories -- what event types scripts can respond to.
 *
 * Each hook type corresponds to a specific gameplay moment where scripts
 * can observe or modify behavior. The engine dispatches these at well-defined
 * points in the game loop.
 */
    enum class HookType : uint32_t
    {
        // Entity lifecycle
        OnEntityCreated,
        OnEntityDestroyed,
        OnEntitySpawned,

        // Combat
        OnDamageDealt, ///< (attacker, target, damage, school) -- can modify damage
        OnDamageTaken, ///< (target, attacker, damage, school) -- can modify damage
        OnHeal,        ///< (healer, target, amount) -- can modify amount
        OnKill,        ///< (killer, victim)
        OnDeath,       ///< (entity, killer)
        OnAbilityCast, ///< (caster, abilityId, target) -- can cancel
        OnAbilityHit,  ///< (caster, target, abilityId, effectIndex)

        // Aura / buff
        OnAuraApplied, ///< (target, auraId, caster)
        OnAuraRemoved, ///< (target, auraId)
        OnAuraTick,    ///< (target, auraId, tickValue)

        // Movement
        OnEntityMoved, ///< (entity, oldPos, newPos)
        OnAreaEnter,   ///< (entity, areaId)
        OnAreaLeave,   ///< (entity, areaId)

        // Interaction
        OnInteract,       ///< (player, target)
        OnDialogueStart,  ///< (player, npcEntity)
        OnDialogueChoice, ///< (player, npcEntity, choiceIndex)

        // Quest
        OnQuestAccepted,  ///< (player, questId)
        OnQuestCompleted, ///< (player, questId)
        OnQuestObjective, ///< (player, questId, objectiveIndex)

        // World
        OnPlayerJoinWorld,  ///< (player)
        OnPlayerLeaveWorld, ///< (player)
        OnWorldUpdate,      ///< (deltaTime)

        // Instance / Encounter
        OnEncounterStart, ///< (instanceId, encounterId)
        OnEncounterEnd,   ///< (instanceId, encounterId, success)

        Count
    };

    // =============================================================================
    // Hook Context
    // =============================================================================

    /**
 * @brief Data passed to script hook handlers.
 *
 * The engine fills in the relevant fields before dispatching. Handlers may
 * read any field and write to `cancelled`, `modifiedValue`, or `valueModified`
 * to influence the outcome.
 */
    struct HookContext
    {
        HookType type{};           ///< Which hook point triggered this dispatch.
        EntityID sourceEntity = 0; ///< Entity that initiated the action (e.g. attacker).
        EntityID targetEntity = 0; ///< Entity receiving the action (e.g. target being damaged).
        uint32_t param1 = 0;       ///< Generic integer parameter (meaning depends on hook type).
        uint32_t param2 = 0;       ///< Second generic integer parameter.
        float floatParam1 = 0.0f;  ///< Generic float parameter (e.g. damage amount).
        float floatParam2 = 0.0f;  ///< Second generic float parameter (e.g. distance).
        std::string stringParam;   ///< Generic string parameter (e.g. weapon name).

        bool cancelled = false;     ///< Script can set this to cancel the action
        float modifiedValue = 0.0f; ///< Script can modify damage/heal amounts
        bool valueModified = false; ///< Set to true when modifiedValue is written
    };

    // =============================================================================
    // Hook Registration
    // =============================================================================

    /** @brief Signature for hook handler callbacks. */
    using HookHandler = std::function<void(HookContext&)>;

    /**
 * @brief A single registered hook handler with metadata.
 */
    struct HookRegistration
    {
        uint32_t id = 0;
        std::string scriptName; ///< Which script registered this
        HookHandler handler;
        int priority = 0; ///< Lower value = runs first
    };

    // =============================================================================
    // ScriptHookManager
    // =============================================================================

    /**
 * @brief Central hook dispatcher -- scripts register handlers, engine dispatches events.
 *
 * Singleton. The engine calls DispatchHook (or convenience wrappers) at gameplay
 * event points. Scripts register handlers via RegisterHook. Handlers run in
 * priority order (lowest first). All methods are thread-safe.
 */
    class ScriptHookManager
    {
      public:
        /** @brief Get the singleton instance. */
        static ScriptHookManager& GetInstance();

        /**
     * @brief Register a hook handler for a specific event type.
     * @param type       The hook type to listen for.
     * @param scriptName Name of the script registering (for hot-reload cleanup).
     * @param handler    Callback invoked when the hook fires.
     * @param priority   Execution order; lower values run first. Default 0.
     * @return Registration ID for later removal via UnregisterHook.
     */
        uint32_t RegisterHook(HookType type, const std::string& scriptName, HookHandler handler, int priority = 0);

        /** @brief Remove a specific hook registration by ID. */
        void UnregisterHook(uint32_t registrationId);

        /**
     * @brief Remove all hooks registered by a specific script.
     *
     * Called during hot-reload: before recompiling a script module, all its
     * hooks are removed so the reloaded version can re-register fresh handlers.
     *
     * @param scriptName The script name passed during RegisterHook.
     */
        void UnregisterAllForScript(const std::string& scriptName);

        /**
     * @brief Dispatch a hook to all registered handlers.
     *
     * Handlers run in priority order. If any handler sets cancelled=true,
     * subsequent handlers still run but the caller should check cancelled.
     *
     * @param type    The hook type to dispatch.
     * @param context Pre-filled context with event data.
     * @return The final HookContext after all handlers have processed it.
     */
        HookContext DispatchHook(HookType type, HookContext context);

        /**
     * @brief Convenience: dispatch a combat-related hook.
     * @param type   Hook type (OnDamageDealt, OnDamageTaken, OnHeal, etc.).
     * @param source The attacking/healing entity.
     * @param target The entity receiving damage/healing.
     * @param value  The damage or heal amount (stored in floatParam1).
     * @param school Damage school or spell school (stored in param1).
     * @return The final HookContext.
     */
        HookContext DispatchCombatHook(HookType type, EntityID source, EntityID target, float value,
                                       uint32_t school = 0);

        /**
     * @brief Convenience: dispatch an entity-centric hook.
     * @param type   Hook type (OnEntityCreated, OnKill, OnAreaEnter, etc.).
     * @param entity The primary entity involved.
     * @param param  Optional parameter (areaId, questId, etc.).
     * @return The final HookContext.
     */
        HookContext DispatchEntityHook(HookType type, EntityID entity, uint32_t param = 0);

        /** @brief Check if any scripts are listening to a specific hook type. */
        bool HasHandlers(HookType type) const;

        /** @brief Total number of registered handlers across all hook types. */
        int GetTotalHandlerCount() const;

        /** @brief Number of registered handlers for a specific hook type. */
        int GetHandlerCount(HookType type) const;

        /** @brief Remove all registered hooks (used during full shutdown). */
        void Clear();

      private:
        ScriptHookManager() = default;

        mutable std::mutex m_mutex;
        std::unordered_map<HookType, std::vector<HookRegistration>> m_hooks;
        uint32_t m_nextId = 1;
    };

} // namespace Spark::Scripting
