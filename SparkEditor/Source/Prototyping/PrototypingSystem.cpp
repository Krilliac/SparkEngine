/**
 * @file PrototypingSystem.cpp
 * @brief Rapid prototyping tools implementation
 */

#include "PrototypingSystem.h"

#include <algorithm>

namespace SparkEditor
{

    void PrototypingSystem::Initialize()
    {
        RegisterBuiltinTemplates();
        m_initialized = true;
    }

    void PrototypingSystem::Update(float deltaTime)
    {
        if (!m_initialized)
            return;

        if (m_isPlaying)
            m_playTime += deltaTime;
    }

    void PrototypingSystem::Shutdown()
    {
        m_primitives.clear();
        m_templates.clear();
        m_rules.clear();
        m_initialized = false;
    }

    uint32_t PrototypingSystem::PlaceBlockout(BlockoutShape shape, float x, float y, float z)
    {
        BlockoutPrimitive prim;
        prim.primitiveId = m_nextPrimitiveId++;
        prim.shape = shape;
        prim.posX = x;
        prim.posY = y;
        prim.posZ = z;
        prim.name = MakeBlockoutName(shape, prim.primitiveId);

        // Shape-specific default scales
        if (shape == BlockoutShape::Stairs)
            prim.scaleY = 2.0f;
        else if (shape == BlockoutShape::Arch)
        {
            prim.scaleX = 2.0f;
            prim.scaleY = 3.0f;
        }
        else if (shape == BlockoutShape::Pipe)
            prim.scaleY = 4.0f;

        m_primitives.push_back(prim);
        return prim.primitiveId;
    }

    bool PrototypingSystem::RemoveBlockout(uint32_t primitiveId)
    {
        auto it = std::find_if(m_primitives.begin(), m_primitives.end(),
                               [primitiveId](const BlockoutPrimitive& p) { return p.primitiveId == primitiveId; });
        if (it == m_primitives.end())
            return false;
        m_primitives.erase(it);
        return true;
    }

    bool PrototypingSystem::ScaleBlockout(uint32_t primitiveId, float sx, float sy, float sz)
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

    void PrototypingSystem::ClearAllBlockouts()
    {
        m_primitives.clear();
    }

    const BlockoutPrimitive* PrototypingSystem::GetBlockout(uint32_t primitiveId) const
    {
        for (const auto& p : m_primitives)
        {
            if (p.primitiveId == primitiveId)
                return &p;
        }
        return nullptr;
    }

    uint32_t PrototypingSystem::ApplyTemplate(GameTemplate type)
    {
        for (const auto& t : m_templates)
        {
            if (t.type == type)
                return t.templateId;
        }
        return 0;
    }

    const GameTemplateConfig* PrototypingSystem::GetTemplate(GameTemplate type) const
    {
        for (const auto& t : m_templates)
        {
            if (t.type == type)
                return &t;
        }
        return nullptr;
    }

    uint32_t PrototypingSystem::AddRule(const std::string& name, const std::string& trigger, const std::string& action,
                                        const std::string& param)
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

    bool PrototypingSystem::RemoveRule(uint32_t ruleId)
    {
        auto it = std::find_if(m_rules.begin(), m_rules.end(),
                               [ruleId](const GameplayRule& r) { return r.ruleId == ruleId; });
        if (it == m_rules.end())
            return false;
        m_rules.erase(it);
        return true;
    }

    bool PrototypingSystem::ToggleRule(uint32_t ruleId)
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

    bool PrototypingSystem::StartPlayTest()
    {
        if (m_isPlaying)
            return false;
        m_isPlaying = true;
        m_playTime = 0.0f;
        return true;
    }

    bool PrototypingSystem::StopPlayTest()
    {
        if (!m_isPlaying)
            return false;
        m_isPlaying = false;
        return true;
    }

    std::string PrototypingSystem::MakeBlockoutName(BlockoutShape shape, uint32_t id) const
    {
        static const char* shapeNames[] = {"Cube", "Cylinder", "Sphere", "Ramp", "Stairs",
                                           "Arch", "LShape",   "TShape", "Ring", "Pipe"};
        int idx = static_cast<int>(shape);
        if (idx >= 0 && idx < static_cast<int>(BlockoutShape::Count))
            return std::string(shapeNames[idx]) + "_" + std::to_string(id);
        return "Prim_" + std::to_string(id);
    }

    void PrototypingSystem::RegisterBuiltinTemplates()
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

} // namespace SparkEditor
