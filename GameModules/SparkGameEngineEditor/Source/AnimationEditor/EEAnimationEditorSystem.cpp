/**
 * @file EEAnimationEditorSystem.cpp
 * @brief Animation state machine editor with blend trees and IK setup
 *
 * Implements animation controller editing with state machines, transitions,
 * blend parameters, IK chains, and preset templates.
 */

#include "EEAnimationEditorSystem.h"
#include "Utils/SparkConsole.h"
#include "Utils/LogMacros.h"

#include <algorithm>

namespace EngineEditor
{

    bool EEAnimationEditorSystem::Initialize(Spark::IEngineContext* context)
    {
        if (!context)
            return false;

        m_context = context;
        RegisterBuiltinPresets();

        m_initialized = true;
        SPARK_LOG_INFO(Spark::LogCategory::Game, "Animation editor system initialized (%zu controllers)",
                       m_controllers.size());
        return true;
    }

    void EEAnimationEditorSystem::Update([[maybe_unused]] float deltaTime)
    {
        if (!m_initialized)
            return;
    }

    void EEAnimationEditorSystem::Shutdown()
    {
        m_controllers.clear();
        m_initialized = false;
    }

    void EEAnimationEditorSystem::RenderDebugUI() {}

    uint32_t EEAnimationEditorSystem::CreateController(const std::string& name)
    {
        AnimController ctrl;
        ctrl.controllerId = m_nextControllerId++;
        ctrl.name = name;

        // Add default base layer
        AnimLayer baseLayer;
        baseLayer.layerId = m_nextLayerId++;
        baseLayer.name = "Base Layer";
        baseLayer.blendMode = AnimBlendMode::Override;
        ctrl.layers.push_back(std::move(baseLayer));

        m_controllers.push_back(std::move(ctrl));
        return m_controllers.back().controllerId;
    }

    bool EEAnimationEditorSystem::DeleteController(uint32_t controllerId)
    {
        auto it = std::find_if(m_controllers.begin(), m_controllers.end(),
                               [controllerId](const AnimController& c) { return c.controllerId == controllerId; });
        if (it == m_controllers.end())
            return false;
        m_controllers.erase(it);
        return true;
    }

    uint32_t EEAnimationEditorSystem::RegisterClip(uint32_t controllerId, const std::string& name,
                                                   const std::string& path, float duration, bool looping)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId)
            {
                AnimClip clip;
                clip.clipId = m_nextClipId++;
                clip.name = name;
                clip.assetPath = path;
                clip.duration = duration;
                clip.isLooping = looping;
                ctrl.clips.push_back(clip);
                return clip.clipId;
            }
        }
        return 0;
    }

    uint32_t EEAnimationEditorSystem::AddState(uint32_t controllerId, uint32_t layerIdx, const std::string& name,
                                               uint32_t clipId)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId && layerIdx < ctrl.layers.size())
            {
                AnimState state;
                state.stateId = m_nextStateId++;
                state.name = name;
                state.clipId = clipId;
                state.isDefault = ctrl.layers[layerIdx].states.empty();
                ctrl.layers[layerIdx].states.push_back(state);
                return state.stateId;
            }
        }
        return 0;
    }

    bool EEAnimationEditorSystem::RemoveState(uint32_t controllerId, uint32_t layerIdx, uint32_t stateId)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId && layerIdx < ctrl.layers.size())
            {
                auto& states = ctrl.layers[layerIdx].states;
                auto it = std::find_if(states.begin(), states.end(),
                                       [stateId](const AnimState& s) { return s.stateId == stateId; });
                if (it == states.end())
                    return false;

                // Remove transitions involving this state
                auto& trans = ctrl.layers[layerIdx].transitions;
                std::erase_if(trans, [stateId](const AnimTransition& t)
                              { return t.fromStateId == stateId || t.toStateId == stateId; });

                states.erase(it);
                return true;
            }
        }
        return false;
    }

    uint32_t EEAnimationEditorSystem::AddTransition(uint32_t controllerId, uint32_t layerIdx, uint32_t fromState,
                                                    uint32_t toState, TransitionCondition cond,
                                                    const std::string& param)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId && layerIdx < ctrl.layers.size())
            {
                AnimTransition t;
                t.transitionId = m_nextTransitionId++;
                t.fromStateId = fromState;
                t.toStateId = toState;
                t.condition = cond;
                t.paramName = param;
                ctrl.layers[layerIdx].transitions.push_back(t);
                return t.transitionId;
            }
        }
        return 0;
    }

    uint32_t EEAnimationEditorSystem::AddParameter(uint32_t controllerId, const std::string& name, PinType type)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId)
            {
                AnimParameter param;
                param.paramId = m_nextParamId++;
                param.name = name;
                param.type = type;
                ctrl.parameters.push_back(param);
                return param.paramId;
            }
        }
        return 0;
    }

    bool EEAnimationEditorSystem::SetParameterFloat(uint32_t controllerId, const std::string& name, float value)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId)
            {
                for (auto& p : ctrl.parameters)
                {
                    if (p.name == name && p.type == PinType::Float)
                    {
                        p.valueFloat = value;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool EEAnimationEditorSystem::SetParameterBool(uint32_t controllerId, const std::string& name, bool value)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId)
            {
                for (auto& p : ctrl.parameters)
                {
                    if (p.name == name && p.type == PinType::Bool)
                    {
                        p.valueBool = value;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    uint32_t EEAnimationEditorSystem::AddIKChain(uint32_t controllerId, const std::string& name, IKSolverType solver,
                                                 const std::string& root, const std::string& tip)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId)
            {
                IKChain chain;
                chain.chainId = m_nextChainId++;
                chain.name = name;
                chain.solver = solver;
                chain.rootBone = root;
                chain.tipBone = tip;
                ctrl.ikChains.push_back(chain);
                return chain.chainId;
            }
        }
        return 0;
    }

    uint32_t EEAnimationEditorSystem::AddLayer(uint32_t controllerId, const std::string& name, AnimBlendMode mode)
    {
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == controllerId)
            {
                AnimLayer layer;
                layer.layerId = m_nextLayerId++;
                layer.name = name;
                layer.blendMode = mode;
                ctrl.layers.push_back(std::move(layer));
                return layer.layerId;
            }
        }
        return 0;
    }

    uint32_t EEAnimationEditorSystem::CreatePresetLocomotion(const std::string& name)
    {
        uint32_t id = CreateController(name);
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == id)
            {
                // Register clips
                uint32_t idleClip = RegisterClip(id, "Idle", "anims/idle.anim", 2.0f, true);
                uint32_t walkClip = RegisterClip(id, "Walk", "anims/walk.anim", 1.0f, true);
                uint32_t runClip = RegisterClip(id, "Run", "anims/run.anim", 0.8f, true);
                uint32_t jumpClip = RegisterClip(id, "Jump", "anims/jump.anim", 0.6f, false);
                uint32_t fallClip = RegisterClip(id, "Fall", "anims/fall.anim", 1.0f, true);
                uint32_t landClip = RegisterClip(id, "Land", "anims/land.anim", 0.3f, false);

                // Parameters
                AddParameter(id, "Speed", PinType::Float);
                AddParameter(id, "IsGrounded", PinType::Bool);
                AddParameter(id, "Jump", PinType::Bool);

                // States in base layer (index 0)
                uint32_t idleState = AddState(id, 0, "Idle", idleClip);
                uint32_t walkState = AddState(id, 0, "Walk", walkClip);
                uint32_t runState = AddState(id, 0, "Run", runClip);
                uint32_t jumpState = AddState(id, 0, "Jump", jumpClip);
                uint32_t fallState = AddState(id, 0, "Fall", fallClip);
                uint32_t landState = AddState(id, 0, "Land", landClip);

                // Transitions
                AddTransition(id, 0, idleState, walkState, TransitionCondition::FloatThreshold, "Speed");
                AddTransition(id, 0, walkState, idleState, TransitionCondition::FloatThreshold, "Speed");
                AddTransition(id, 0, walkState, runState, TransitionCondition::FloatThreshold, "Speed");
                AddTransition(id, 0, runState, walkState, TransitionCondition::FloatThreshold, "Speed");
                AddTransition(id, 0, idleState, jumpState, TransitionCondition::BoolParam, "Jump");
                AddTransition(id, 0, walkState, jumpState, TransitionCondition::BoolParam, "Jump");
                AddTransition(id, 0, runState, jumpState, TransitionCondition::BoolParam, "Jump");
                AddTransition(id, 0, jumpState, fallState, TransitionCondition::AnimFinished, "");
                AddTransition(id, 0, fallState, landState, TransitionCondition::BoolParam, "IsGrounded");
                AddTransition(id, 0, landState, idleState, TransitionCondition::AnimFinished, "");

                // IK chains
                AddIKChain(id, "LeftFoot", IKSolverType::TwoBone, "LeftUpLeg", "LeftFoot");
                AddIKChain(id, "RightFoot", IKSolverType::TwoBone, "RightUpLeg", "RightFoot");

                break;
            }
        }
        return id;
    }

    uint32_t EEAnimationEditorSystem::CreatePresetCombat(const std::string& name)
    {
        uint32_t id = CreateController(name);
        for (auto& ctrl : m_controllers)
        {
            if (ctrl.controllerId == id)
            {
                uint32_t idleClip = RegisterClip(id, "CombatIdle", "anims/combat_idle.anim", 1.5f, true);
                uint32_t attackClip = RegisterClip(id, "Attack", "anims/attack.anim", 0.5f, false);
                uint32_t heavyClip = RegisterClip(id, "HeavyAttack", "anims/heavy_attack.anim", 0.8f, false);
                uint32_t blockClip = RegisterClip(id, "Block", "anims/block.anim", 1.0f, true);
                uint32_t dodgeClip = RegisterClip(id, "Dodge", "anims/dodge.anim", 0.4f, false);
                uint32_t hitClip = RegisterClip(id, "Hit", "anims/hit_react.anim", 0.3f, false);

                AddParameter(id, "Attack", PinType::Bool);
                AddParameter(id, "HeavyAttack", PinType::Bool);
                AddParameter(id, "Block", PinType::Bool);
                AddParameter(id, "Dodge", PinType::Bool);
                AddParameter(id, "Hit", PinType::Bool);

                uint32_t idleState = AddState(id, 0, "CombatIdle", idleClip);
                uint32_t attackState = AddState(id, 0, "Attack", attackClip);
                uint32_t heavyState = AddState(id, 0, "HeavyAttack", heavyClip);
                uint32_t blockState = AddState(id, 0, "Block", blockClip);
                uint32_t dodgeState = AddState(id, 0, "Dodge", dodgeClip);
                uint32_t hitState = AddState(id, 0, "Hit", hitClip);

                AddTransition(id, 0, idleState, attackState, TransitionCondition::BoolParam, "Attack");
                AddTransition(id, 0, idleState, heavyState, TransitionCondition::BoolParam, "HeavyAttack");
                AddTransition(id, 0, idleState, blockState, TransitionCondition::BoolParam, "Block");
                AddTransition(id, 0, idleState, dodgeState, TransitionCondition::BoolParam, "Dodge");
                AddTransition(id, 0, attackState, idleState, TransitionCondition::AnimFinished, "");
                AddTransition(id, 0, heavyState, idleState, TransitionCondition::AnimFinished, "");
                AddTransition(id, 0, blockState, idleState, TransitionCondition::BoolParam, "Block");
                AddTransition(id, 0, dodgeState, idleState, TransitionCondition::AnimFinished, "");
                AddTransition(id, 0, idleState, hitState, TransitionCondition::BoolParam, "Hit");
                AddTransition(id, 0, hitState, idleState, TransitionCondition::AnimFinished, "");

                // Upper body layer for attack while moving
                AddLayer(id, "UpperBody", AnimBlendMode::Additive);

                // Aim IK
                AddIKChain(id, "AimLook", IKSolverType::LookAt, "Spine", "Head");

                break;
            }
        }
        return id;
    }

    std::string EEAnimationEditorSystem::GetControllerListString() const
    {
        std::string s = "=== Animation Controllers ===\n";
        for (const auto& c : m_controllers)
        {
            s += "  [" + std::to_string(c.controllerId) + "] " + c.name;
            s += " (layers:" + std::to_string(c.layers.size());
            s += " clips:" + std::to_string(c.clips.size());
            s += " params:" + std::to_string(c.parameters.size());
            s += " ik:" + std::to_string(c.ikChains.size()) + ")\n";
        }
        if (m_controllers.empty())
            s += "  (none)\n";
        return s;
    }

    std::string EEAnimationEditorSystem::GetControllerDetailString(uint32_t controllerId) const
    {
        for (const auto& c : m_controllers)
        {
            if (c.controllerId == controllerId)
            {
                std::string s = "=== Controller: " + c.name + " ===\n";
                s += "Clips: " + std::to_string(c.clips.size()) + "\n";
                for (const auto& clip : c.clips)
                    s += "  " + clip.name + " (" + std::to_string(clip.duration) + "s" +
                         (clip.isLooping ? ", loop" : "") + ")\n";

                s += "Parameters: " + std::to_string(c.parameters.size()) + "\n";
                for (const auto& p : c.parameters)
                    s += "  " + p.name + " (type=" + std::to_string(static_cast<int>(p.type)) + ")\n";

                for (size_t i = 0; i < c.layers.size(); i++)
                {
                    const auto& l = c.layers[i];
                    s += "Layer " + std::to_string(i) + ": " + l.name +
                         " (blend=" + std::to_string(static_cast<int>(l.blendMode)) + ")\n";
                    s += "  States: " + std::to_string(l.states.size()) + "\n";
                    for (const auto& st : l.states)
                        s += "    [" + std::to_string(st.stateId) + "] " + st.name +
                             (st.isDefault ? " [DEFAULT]" : "") + "\n";
                    s += "  Transitions: " + std::to_string(l.transitions.size()) + "\n";
                }

                s += "IK Chains: " + std::to_string(c.ikChains.size()) + "\n";
                for (const auto& ik : c.ikChains)
                    s += "  " + ik.name + " (" + std::to_string(static_cast<int>(ik.solver)) + " " + ik.rootBone +
                         " -> " + ik.tipBone + ")\n";
                return s;
            }
        }
        return "Controller not found";
    }

    std::string EEAnimationEditorSystem::GetIKSolverCatalog() const
    {
        return "IK Solvers: TwoBone, FABRIK, CCD, LookAt, SplineIK\n";
    }

    void EEAnimationEditorSystem::RegisterBuiltinPresets()
    {
        CreatePresetLocomotion("AC_Locomotion");
        CreatePresetCombat("AC_Combat");
    }

} // namespace EngineEditor
