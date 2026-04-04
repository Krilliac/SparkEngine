/**
 * @file GameplaySystemExtension.h
 * @brief Extension point system for game modules to extend engine quest/dialogue
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides interfaces that game modules (RPG, MMO, etc.) implement to add
 * genre-specific quest types and dialogue actions without reimplementing the
 * entire engine QuestSystem or DialogueSystem.
 *
 * @code
 *   // In your game module's initialization:
 *   auto ext = std::make_unique<MyRPGQuestExtension>();
 *   GameplayExtensionRegistry::GetInstance().RegisterQuestExtension(std::move(ext));
 * @endcode
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Spark::Gameplay
{

    // ============================================================================
    // Quest Extension Types
    // ============================================================================

    /**
     * @brief Minimal quest definition passed to extensions
     */
    struct QuestDefinition
    {
        uint32_t questId = 0;
        std::string name;
        std::string typeName;
        std::vector<std::string> objectiveIds;
    };

    /**
     * @brief Mutable per-entity quest state passed to extensions
     */
    struct QuestContext
    {
        uint32_t entityId = 0;
        uint32_t questId = 0;
        bool isActive = false;
        bool isComplete = false;
        std::vector<int32_t> objectiveProgress;
    };

    // ============================================================================
    // IQuestTypeExtension
    // ============================================================================

    /**
     * @brief Base class for game-specific quest type extensions
     *
     * Implement this to add new quest types (e.g. "escort", "crafting",
     * "faction_reputation") that the engine QuestSystem delegates to
     * when it encounters a quest with a matching type name.
     */
    class IQuestTypeExtension
    {
      public:
        virtual ~IQuestTypeExtension() = default;

        /** @brief Unique type name this extension handles (e.g. "escort") */
        virtual std::string GetTypeName() const = 0;

        /** @brief Check whether the quest can be started in the given context */
        virtual bool CanStart(const QuestDefinition& quest, const QuestContext& ctx) const = 0;

        /** @brief Called when the quest is started */
        virtual void OnStarted(const QuestDefinition& quest, QuestContext& ctx) = 0;

        /**
         * @brief Called when objective progress is reported
         * @param quest        Quest definition
         * @param ctx          Mutable quest context
         * @param objectiveId  Which objective progressed
         * @param progress     Amount of progress to add
         */
        virtual void OnObjectiveProgress(const QuestDefinition& quest, QuestContext& ctx,
                                         const std::string& objectiveId, int32_t progress) = 0;

        /** @brief Check whether all objectives are satisfied */
        virtual bool IsComplete(const QuestDefinition& quest, const QuestContext& ctx) const = 0;

        /** @brief Called when the quest is completed (award rewards, etc.) */
        virtual void OnCompleted(const QuestDefinition& quest, QuestContext& ctx) = 0;
    };

    // ============================================================================
    // IDialogueExtension
    // ============================================================================

    /**
     * @brief Base class for dialogue condition/action extensions
     *
     * Implement this to add game-specific dialogue conditions (e.g. "has_item",
     * "faction_standing") and actions (e.g. "give_item", "start_quest") that
     * the engine DialogueSystem delegates to during dialogue evaluation.
     */
    class IDialogueExtension
    {
      public:
        virtual ~IDialogueExtension() = default;

        /** @brief Unique name for this extension (e.g. "rpg_conditions") */
        virtual std::string GetExtensionName() const = 0;

        /**
         * @brief Evaluate a custom condition during dialogue
         * @param condType  Condition type identifier (e.g. "has_item")
         * @param params    Serialized parameters for the condition
         * @return true if the condition is met
         */
        virtual bool EvaluateCondition(const std::string& condType, const std::string& params) const = 0;

        /**
         * @brief Execute a custom action during dialogue
         * @param actionType  Action type identifier (e.g. "give_item")
         * @param params      Serialized parameters for the action
         */
        virtual void ExecuteAction(const std::string& actionType, const std::string& params) = 0;
    };

    // ============================================================================
    // GameplayExtensionRegistry
    // ============================================================================

    /**
     * @brief Registry for game module extensions to engine gameplay systems
     *
     * Singleton that game modules register their quest/dialogue extensions with.
     * The engine QuestSystem and DialogueSystem query this registry to delegate
     * genre-specific behavior to the appropriate game module.
     */
    class GameplayExtensionRegistry
    {
      public:
        /**
         * @brief Get the singleton instance
         * @return Reference to the global registry
         */
        static GameplayExtensionRegistry& GetInstance()
        {
            static GameplayExtensionRegistry instance;
            return instance;
        }

        /**
         * @brief Register a quest type extension
         * @param ext  Ownership is transferred to the registry
         */
        void RegisterQuestExtension(std::unique_ptr<IQuestTypeExtension> ext)
        {
            if (ext)
            {
                m_questExtensions.push_back(std::move(ext));
            }
        }

        /**
         * @brief Register a dialogue extension
         * @param ext  Ownership is transferred to the registry
         */
        void RegisterDialogueExtension(std::unique_ptr<IDialogueExtension> ext)
        {
            if (ext)
            {
                m_dialogueExtensions.push_back(std::move(ext));
            }
        }

        /**
         * @brief Look up a quest extension by type name
         * @param typeName  The quest type to find (e.g. "escort")
         * @return Non-owning pointer to the extension, or nullptr if not found
         */
        IQuestTypeExtension* GetQuestExtension(const std::string& typeName) const
        {
            for (const auto& ext : m_questExtensions)
            {
                if (ext->GetTypeName() == typeName)
                {
                    return ext.get();
                }
            }
            return nullptr;
        }

        /**
         * @brief Look up a dialogue extension by name
         * @param name  The extension name to find
         * @return Non-owning pointer to the extension, or nullptr if not found
         */
        IDialogueExtension* GetDialogueExtension(const std::string& name) const
        {
            for (const auto& ext : m_dialogueExtensions)
            {
                if (ext->GetExtensionName() == name)
                {
                    return ext.get();
                }
            }
            return nullptr;
        }

        /**
         * @brief Get all registered quest type names
         * @return Vector of type name strings
         */
        std::vector<std::string> GetRegisteredQuestTypes() const
        {
            std::vector<std::string> types;
            types.reserve(m_questExtensions.size());
            for (const auto& ext : m_questExtensions)
            {
                types.push_back(ext->GetTypeName());
            }
            return types;
        }

        /**
         * @brief Get all registered dialogue extension names
         * @return Vector of extension name strings
         */
        std::vector<std::string> GetRegisteredDialogueExtensions() const
        {
            std::vector<std::string> names;
            names.reserve(m_dialogueExtensions.size());
            for (const auto& ext : m_dialogueExtensions)
            {
                names.push_back(ext->GetExtensionName());
            }
            return names;
        }

        /**
         * @brief Clear all registered extensions (for hot-reload)
         */
        void Clear()
        {
            m_questExtensions.clear();
            m_dialogueExtensions.clear();
        }

      private:
        GameplayExtensionRegistry() = default;

        std::vector<std::unique_ptr<IQuestTypeExtension>> m_questExtensions;
        std::vector<std::unique_ptr<IDialogueExtension>> m_dialogueExtensions;
    };

} // namespace Spark::Gameplay
