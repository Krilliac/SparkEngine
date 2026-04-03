/**
 * @file EEPrototypingSystem.h
 * @brief Rapid prototyping tools: blockout meshes, game templates, quick iteration
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides rapid prototyping tools including primitive blockout shapes for
 * level gray-boxing, pre-built game templates (FPS, third-person, top-down,
 * etc.), quick-play testing, and gameplay rule configuration.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EngineEditor
{

    /// @brief A placed blockout primitive for level gray-boxing
    struct BlockoutPrimitive
    {
        uint32_t primitiveId = 0;
        std::string name;
        BlockoutShape shape = BlockoutShape::Cube;
        float posX = 0.0f, posY = 0.0f, posZ = 0.0f;
        float rotY = 0.0f;
        float scaleX = 1.0f, scaleY = 1.0f, scaleZ = 1.0f;
        float colorR = 0.7f, colorG = 0.7f, colorB = 0.7f;
        bool hasCollision = true;
    };

    /// @brief A game template with pre-configured systems
    struct GameTemplateConfig
    {
        uint32_t templateId = 0;
        std::string name;
        std::string description;
        GameTemplate type = GameTemplate::BlankProject;
        bool includesCamera = true;
        bool includesPlayer = true;
        bool includesPhysics = true;
        bool includesAI = false;
        bool includesUI = true;
        bool includesNetworking = false;
        uint32_t defaultEntityCount = 0;
    };

    /// @brief A gameplay rule (no-code game logic)
    struct GameplayRule
    {
        uint32_t ruleId = 0;
        std::string name;
        std::string triggerEvent; ///< "OnCollision", "OnTimer", "OnInput", "OnOverlap"
        std::string actionType;   ///< "SpawnEntity", "PlaySound", "AddScore", "SetVariable"
        std::string actionParam;
        bool isEnabled = true;
    };

    /// @brief A prototype session for quick play-testing
    struct PrototypeSession
    {
        uint32_t sessionId = 0;
        std::string name;
        GameTemplate baseTemplate = GameTemplate::BlankProject;
        std::vector<BlockoutPrimitive> primitives;
        std::vector<GameplayRule> rules;
        float playTime = 0.0f;
        bool isPlaying = false;
    };

    /**
     * @brief Rapid prototyping toolkit for no-code game iteration
     *
     * Manages blockout primitives, game templates, gameplay rules,
     * and prototype sessions with play-testing.
     */
    class EEPrototypingSystem
    {
      public:
        EEPrototypingSystem() = default;
        ~EEPrototypingSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Blockout primitives
        uint32_t PlaceBlockout(BlockoutShape shape, float x, float y, float z);
        bool RemoveBlockout(uint32_t primitiveId);
        bool ScaleBlockout(uint32_t primitiveId, float sx, float sy, float sz);
        void ClearAllBlockouts();

        // Templates
        uint32_t ApplyTemplate(GameTemplate type);
        const GameTemplateConfig* GetTemplate(GameTemplate type) const;

        // Gameplay rules
        uint32_t AddRule(const std::string& name, const std::string& trigger, const std::string& action,
                         const std::string& param);
        bool RemoveRule(uint32_t ruleId);
        bool ToggleRule(uint32_t ruleId);

        // Prototype session
        bool StartPlayTest();
        bool StopPlayTest();
        bool IsPlaying() const;

        // Queries
        size_t GetBlockoutCount() const { return m_primitives.size(); }
        size_t GetTemplateCount() const { return m_templates.size(); }
        size_t GetRuleCount() const { return m_rules.size(); }
        std::string GetBlockoutListString() const;
        std::string GetTemplateListString() const;
        std::string GetRuleListString() const;
        std::string GetSessionStatusString() const;

      private:
        void RegisterBuiltinTemplates();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<BlockoutPrimitive> m_primitives;
        std::vector<GameTemplateConfig> m_templates;
        std::vector<GameplayRule> m_rules;
        PrototypeSession m_session;
        uint32_t m_nextPrimitiveId{1};
        uint32_t m_nextRuleId{1};
        uint32_t m_nextSessionId{1};
        bool m_initialized{false};
    };

} // namespace EngineEditor
