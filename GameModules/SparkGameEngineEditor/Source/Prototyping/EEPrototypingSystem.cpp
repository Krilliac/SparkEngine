/**
 * @file EEPrototypingSystem.cpp
 * @brief Rapid prototyping tools: blockout meshes, game templates, quick iteration
 *
 * Implements rapid prototyping with blockout primitives, pre-built game
 * templates, gameplay rules, and play-test sessions.
 */

#include "EEPrototypingSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace EngineEditor
{

    bool EEPrototypingSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        RegisterBuiltinTemplates();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Prototyping system initialized (%zu templates)", m_templates.size());
        return true;
    }

    void EEPrototypingSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        if (m_session.isPlaying)
        {
            m_session.playTime += deltaTime;

            // Evaluate gameplay rules
            for (const auto& rule : m_rules)
            {
                if (!rule.isEnabled)
                    continue;
                // Rule evaluation dispatched to visual scripting or engine events
                (void)rule;
            }
        }
    }

    void EEPrototypingSystem::Shutdown()
    {
        m_primitives.clear();
        m_templates.clear();
        m_rules.clear();
        m_initialized = false;
    }

    void EEPrototypingSystem::RenderDebugUI() {}

    uint32_t EEPrototypingSystem::PlaceBlockout(BlockoutShape shape, float x, float y, float z)
    {
        BlockoutPrimitive prim;
        prim.primitiveId = m_nextPrimitiveId++;
        prim.shape = shape;
        prim.posX = x;
        prim.posY = y;
        prim.posZ = z;

        switch (shape)
        {
        case BlockoutShape::Cube:
            prim.name = "Cube_" + std::to_string(prim.primitiveId);
            break;
        case BlockoutShape::Cylinder:
            prim.name = "Cylinder_" + std::to_string(prim.primitiveId);
            break;
        case BlockoutShape::Sphere:
            prim.name = "Sphere_" + std::to_string(prim.primitiveId);
            break;
        case BlockoutShape::Ramp:
            prim.name = "Ramp_" + std::to_string(prim.primitiveId);
            break;
        case BlockoutShape::Stairs:
            prim.name = "Stairs_" + std::to_string(prim.primitiveId);
            prim.scaleY = 2.0f;
            break;
        case BlockoutShape::Arch:
            prim.name = "Arch_" + std::to_string(prim.primitiveId);
            prim.scaleX = 2.0f;
            prim.scaleY = 3.0f;
            break;
        case BlockoutShape::LShape:
            prim.name = "LShape_" + std::to_string(prim.primitiveId);
            break;
        case BlockoutShape::TShape:
            prim.name = "TShape_" + std::to_string(prim.primitiveId);
            break;
        case BlockoutShape::Ring:
            prim.name = "Ring_" + std::to_string(prim.primitiveId);
            break;
        case BlockoutShape::Pipe:
            prim.name = "Pipe_" + std::to_string(prim.primitiveId);
            prim.scaleY = 4.0f;
            break;
        default:
            prim.name = "Prim_" + std::to_string(prim.primitiveId);
            break;
        }

        m_primitives.push_back(prim);
        return prim.primitiveId;
    }

    bool EEPrototypingSystem::RemoveBlockout(uint32_t primitiveId)
    {
        auto it = std::find_if(m_primitives.begin(), m_primitives.end(),
                               [primitiveId](const BlockoutPrimitive& p) { return p.primitiveId == primitiveId; });
        if (it == m_primitives.end())
            return false;
        m_primitives.erase(it);
        return true;
    }

    bool EEPrototypingSystem::ScaleBlockout(uint32_t primitiveId, float sx, float sy, float sz)
    {
        for (auto& p : m_primitives)
        {
            if (p.primitiveId == primitiveId)
            {
                p.scaleX = sx;
                p.scaleY = sy;
                p.scaleZ = sz;
                return true;
            }
        }
        return false;
    }

    void EEPrototypingSystem::ClearAllBlockouts()
    {
        m_primitives.clear();
    }

    uint32_t EEPrototypingSystem::ApplyTemplate(GameTemplate type)
    {
        for (const auto& t : m_templates)
        {
            if (t.type == type)
            {
                m_session.baseTemplate = type;
                return t.templateId;
            }
        }
        return 0;
    }

    const GameTemplateConfig* EEPrototypingSystem::GetTemplate(GameTemplate type) const
    {
        for (const auto& t : m_templates)
        {
            if (t.type == type)
                return &t;
        }
        return nullptr;
    }

    uint32_t EEPrototypingSystem::AddRule(const std::string& name, const std::string& trigger,
                                          const std::string& action, const std::string& param)
    {
        GameplayRule rule;
        rule.ruleId = m_nextRuleId++;
        rule.name = name;
        rule.triggerEvent = trigger;
        rule.actionType = action;
        rule.actionParam = param;
        m_rules.push_back(rule);
        return rule.ruleId;
    }

    bool EEPrototypingSystem::RemoveRule(uint32_t ruleId)
    {
        auto it = std::find_if(m_rules.begin(), m_rules.end(),
                               [ruleId](const GameplayRule& r) { return r.ruleId == ruleId; });
        if (it == m_rules.end())
            return false;
        m_rules.erase(it);
        return true;
    }

    bool EEPrototypingSystem::ToggleRule(uint32_t ruleId)
    {
        for (auto& r : m_rules)
        {
            if (r.ruleId == ruleId)
            {
                r.isEnabled = !r.isEnabled;
                return true;
            }
        }
        return false;
    }

    bool EEPrototypingSystem::StartPlayTest()
    {
        if (m_session.isPlaying)
            return false;
        m_session.sessionId = m_nextSessionId++;
        m_session.isPlaying = true;
        m_session.playTime = 0.0f;
        return true;
    }

    bool EEPrototypingSystem::StopPlayTest()
    {
        if (!m_session.isPlaying)
            return false;
        m_session.isPlaying = false;
        return true;
    }

    bool EEPrototypingSystem::IsPlaying() const
    {
        return m_session.isPlaying;
    }

    std::string EEPrototypingSystem::GetBlockoutListString() const
    {
        std::string s = "=== Blockout Primitives (" + std::to_string(m_primitives.size()) + ") ===\n";
        for (const auto& p : m_primitives)
        {
            s += "  [" + std::to_string(p.primitiveId) + "] " + p.name;
            s += " at (" + std::to_string(static_cast<int>(p.posX)) + ", " + std::to_string(static_cast<int>(p.posY)) +
                 ", " + std::to_string(static_cast<int>(p.posZ)) + ")";
            s += " scale(" + std::to_string(p.scaleX).substr(0, 3) + ", " + std::to_string(p.scaleY).substr(0, 3) +
                 ", " + std::to_string(p.scaleZ).substr(0, 3) + ")\n";
        }
        if (m_primitives.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EEPrototypingSystem::GetTemplateListString() const
    {
        std::string s = "=== Game Templates ===\n";
        for (const auto& t : m_templates)
        {
            s += "  [" + std::to_string(t.templateId) + "] " + t.name + "\n";
            s += "    " + t.description + "\n";
            s += "    Camera:" + std::string(t.includesCamera ? "Y" : "N");
            s += " Player:" + std::string(t.includesPlayer ? "Y" : "N");
            s += " Physics:" + std::string(t.includesPhysics ? "Y" : "N");
            s += " AI:" + std::string(t.includesAI ? "Y" : "N");
            s += " UI:" + std::string(t.includesUI ? "Y" : "N");
            s += " Net:" + std::string(t.includesNetworking ? "Y" : "N") + "\n";
        }
        return s;
    }

    std::string EEPrototypingSystem::GetRuleListString() const
    {
        std::string s = "=== Gameplay Rules ===\n";
        for (const auto& r : m_rules)
        {
            s += "  [" + std::to_string(r.ruleId) + "] " + r.name;
            s += " (" + r.triggerEvent + " -> " + r.actionType;
            if (!r.actionParam.empty())
                s += "(" + r.actionParam + ")";
            s += std::string(r.isEnabled ? "" : " [DISABLED]") + ")\n";
        }
        if (m_rules.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EEPrototypingSystem::GetSessionStatusString() const
    {
        std::string s = "=== Prototype Session ===\n";
        s += "Playing: " + std::string(m_session.isPlaying ? "YES" : "NO") + "\n";
        s += "Play Time: " + std::to_string(static_cast<int>(m_session.playTime)) + "s\n";
        s += "Template: " + std::to_string(static_cast<int>(m_session.baseTemplate)) + "\n";
        s += "Blockouts: " + std::to_string(m_primitives.size()) + "\n";
        s += "Rules: " + std::to_string(m_rules.size()) + "\n";
        return s;
    }

    void EEPrototypingSystem::RegisterBuiltinTemplates()
    {
        uint32_t id = 1;
        auto add = [&](const std::string& name, const std::string& desc, GameTemplate type, bool cam, bool player,
                       bool physics, bool ai, bool ui, bool net, uint32_t entities)
        {
            GameTemplateConfig t;
            t.templateId = id++;
            t.name = name;
            t.description = desc;
            t.type = type;
            t.includesCamera = cam;
            t.includesPlayer = player;
            t.includesPhysics = physics;
            t.includesAI = ai;
            t.includesUI = ui;
            t.includesNetworking = net;
            t.defaultEntityCount = entities;
            m_templates.push_back(std::move(t));
        };

        add("Blank Project", "Empty scene with basic camera", GameTemplate::BlankProject, true, false, false, false,
            false, false, 1);
        add("First Person", "FPS controller with physics and basic HUD", GameTemplate::FirstPerson, true, true, true,
            false, true, false, 5);
        add("Third Person", "Third-person camera, character controller, basic HUD", GameTemplate::ThirdPerson, true,
            true, true, false, true, false, 5);
        add("Top Down", "Overhead camera, click-to-move, minimap", GameTemplate::TopDown, true, true, true, true, true,
            false, 10);
        add("Side Scroller", "2D-style camera, platformer physics", GameTemplate::SideScroller, true, true, true, false,
            true, false, 3);
        add("Vehicle Sim", "Vehicle physics, speedometer HUD, track setup", GameTemplate::VehicleSim, true, true, true,
            false, true, false, 8);
        add("Puzzle Game", "Static camera, drag-and-drop interaction, score UI", GameTemplate::PuzzleGame, true, false,
            true, false, true, false, 20);
        add("Twin Stick", "Twin-stick controls, arena spawners, wave system", GameTemplate::TwinStick, true, true, true,
            true, true, false, 15);
    }

} // namespace EngineEditor
