/**
 * @file PrototypingSystem.h
 * @brief Rapid prototyping tools: blockout meshes, game templates, gameplay rules
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides rapid prototyping tools including primitive blockout shapes for
 * level gray-boxing, pre-built game templates, no-code gameplay rules,
 * and quick play-test sessions.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace SparkEditor
{

    /// @brief Prototype primitive shapes for quick level blocking
    enum class BlockoutShape : uint8_t
    {
        Cube = 0,
        Cylinder = 1,
        Sphere = 2,
        Ramp = 3,
        Stairs = 4,
        Arch = 5,
        LShape = 6,
        TShape = 7,
        Ring = 8,
        Pipe = 9,
        Count = 10
    };

    /// @brief Pre-built gameplay templates for rapid prototyping
    enum class GameTemplate : uint8_t
    {
        BlankProject = 0,
        FirstPerson = 1,
        ThirdPerson = 2,
        TopDown = 3,
        SideScroller = 4,
        VehicleSim = 5,
        PuzzleGame = 6,
        TwinStick = 7,
        Count = 8
    };

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

    /**
     * @brief Rapid prototyping toolkit for no-code game iteration
     *
     * Manages blockout primitives, game templates, gameplay rules,
     * and prototype sessions with play-testing.
     */
    class PrototypingSystem
    {
      public:
        PrototypingSystem() = default;
        ~PrototypingSystem() = default;

        void Initialize();
        void Update(float deltaTime);
        void Shutdown();

        // Blockout primitives
        uint32_t PlaceBlockout(BlockoutShape shape, float x, float y, float z);
        bool RemoveBlockout(uint32_t primitiveId);
        bool ScaleBlockout(uint32_t primitiveId, float sx, float sy, float sz);
        void ClearAllBlockouts();
        const BlockoutPrimitive* GetBlockout(uint32_t primitiveId) const;

        // Templates
        uint32_t ApplyTemplate(GameTemplate type);
        const GameTemplateConfig* GetTemplate(GameTemplate type) const;

        // Gameplay rules
        uint32_t AddRule(const std::string& name, const std::string& trigger, const std::string& action,
                         const std::string& param);
        bool RemoveRule(uint32_t ruleId);
        bool ToggleRule(uint32_t ruleId);

        // Play-test
        bool StartPlayTest();
        bool StopPlayTest();
        bool IsPlaying() const { return m_isPlaying; }
        float GetPlayTime() const { return m_playTime; }

        // Queries
        const std::vector<BlockoutPrimitive>& GetBlockouts() const { return m_primitives; }
        const std::vector<GameTemplateConfig>& GetTemplates() const { return m_templates; }
        const std::vector<GameplayRule>& GetRules() const { return m_rules; }

      private:
        void RegisterBuiltinTemplates();
        std::string MakeBlockoutName(BlockoutShape shape, uint32_t id) const;

        std::vector<BlockoutPrimitive> m_primitives;
        std::vector<GameTemplateConfig> m_templates;
        std::vector<GameplayRule> m_rules;
        uint32_t m_nextPrimitiveId{1};
        uint32_t m_nextRuleId{1};
        float m_playTime{0.0f};
        bool m_isPlaying{false};
        bool m_initialized{false};
    };

} // namespace SparkEditor
