/**
 * @file EEAnimationEditorSystem.h
 * @brief Animation state machine editor with blend trees and IK setup
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides visual editing of animation state machines, blend trees with
 * multi-parameter blending, IK chain configuration, animation layer
 * management, and event/notify tracks.
 */

#pragma once

#include "Spark/IEngineContext.h"
#include "Enums/EngineEditorEnums.h"

#include <cstdint>
#include <string>
#include <vector>

namespace EngineEditor
{

    /// @brief An animation clip reference
    struct AnimClip
    {
        uint32_t clipId = 0;
        std::string name;
        std::string assetPath;
        float duration = 1.0f;
        bool isLooping = false;
        bool isAdditive = false;
    };

    /// @brief A state in the animation state machine
    struct AnimState
    {
        uint32_t stateId = 0;
        std::string name;
        uint32_t clipId = 0; ///< Main clip for this state
        float playRate = 1.0f;
        float posX = 0.0f; ///< Graph canvas position
        float posY = 0.0f;
        bool isDefault = false; ///< Entry state
    };

    /// @brief A transition between two states
    struct AnimTransition
    {
        uint32_t transitionId = 0;
        uint32_t fromStateId = 0;
        uint32_t toStateId = 0;
        TransitionCondition condition = TransitionCondition::BoolParam;
        std::string paramName;
        float threshold = 0.0f;
        float blendDuration = 0.25f;
        bool hasExitTime = false;
        float exitTime = 0.9f;
    };

    /// @brief An animation parameter used in transitions and blend trees
    struct AnimParameter
    {
        uint32_t paramId = 0;
        std::string name;
        PinType type = PinType::Float; ///< Bool, Int, Float, or Exec (trigger)
        float valueFloat = 0.0f;
        int valueInt = 0;
        bool valueBool = false;
    };

    /// @brief IK chain configuration
    struct IKChain
    {
        uint32_t chainId = 0;
        std::string name;
        IKSolverType solver = IKSolverType::TwoBone;
        std::string rootBone;
        std::string tipBone;
        std::string targetBone;
        float weight = 1.0f;
        bool isEnabled = true;
    };

    /// @brief An animation layer for layered blending
    struct AnimLayer
    {
        uint32_t layerId = 0;
        std::string name;
        AnimBlendMode blendMode = AnimBlendMode::Override;
        float weight = 1.0f;
        std::string boneMask; ///< Bone mask name (e.g., "UpperBody")
        std::vector<AnimState> states;
        std::vector<AnimTransition> transitions;
    };

    /// @brief A complete animation controller
    struct AnimController
    {
        uint32_t controllerId = 0;
        std::string name;
        std::vector<AnimLayer> layers;
        std::vector<AnimParameter> parameters;
        std::vector<IKChain> ikChains;
        std::vector<AnimClip> clips;
    };

    /**
     * @brief Animation state machine editor for no-code animation setup
     *
     * Manages animation controllers with visual state machines, transitions,
     * blend parameters, IK chains, and layered blending.
     */
    class EEAnimationEditorSystem
    {
      public:
        EEAnimationEditorSystem() = default;
        ~EEAnimationEditorSystem() = default;

        bool Initialize(Spark::IEngineContext* context);
        void Update(float deltaTime);
        void Shutdown();
        void RenderDebugUI();

        // Controller management
        uint32_t CreateController(const std::string& name);
        bool DeleteController(uint32_t controllerId);

        // Clip management
        uint32_t RegisterClip(uint32_t controllerId, const std::string& name, const std::string& path, float duration,
                              bool looping);

        // State machine editing
        uint32_t AddState(uint32_t controllerId, uint32_t layerIdx, const std::string& name, uint32_t clipId);
        bool RemoveState(uint32_t controllerId, uint32_t layerIdx, uint32_t stateId);
        uint32_t AddTransition(uint32_t controllerId, uint32_t layerIdx, uint32_t fromState, uint32_t toState,
                               TransitionCondition cond, const std::string& param);

        // Parameters
        uint32_t AddParameter(uint32_t controllerId, const std::string& name, PinType type);
        bool SetParameterFloat(uint32_t controllerId, const std::string& name, float value);
        bool SetParameterBool(uint32_t controllerId, const std::string& name, bool value);

        // IK
        uint32_t AddIKChain(uint32_t controllerId, const std::string& name, IKSolverType solver,
                            const std::string& root, const std::string& tip);

        // Layers
        uint32_t AddLayer(uint32_t controllerId, const std::string& name, AnimBlendMode mode);

        // Presets
        uint32_t CreatePresetLocomotion(const std::string& name);
        uint32_t CreatePresetCombat(const std::string& name);

        // Queries
        size_t GetControllerCount() const { return m_controllers.size(); }
        std::string GetControllerListString() const;
        std::string GetControllerDetailString(uint32_t controllerId) const;
        std::string GetIKSolverCatalog() const;

      private:
        void RegisterBuiltinPresets();

        Spark::IEngineContext* m_context{nullptr};
        std::vector<AnimController> m_controllers;
        uint32_t m_nextControllerId{1};
        uint32_t m_nextClipId{1};
        uint32_t m_nextStateId{1};
        uint32_t m_nextTransitionId{1};
        uint32_t m_nextParamId{1};
        uint32_t m_nextChainId{1};
        uint32_t m_nextLayerId{1};
        bool m_initialized{false};
    };

} // namespace EngineEditor
