/**
 * @file TutorialSystem.h
 * @brief Interactive editor tutorial system with step-by-step guidance
 * @author Spark Engine Team
 * @date 2026
 *
 * Provides a guided tutorial framework for the SparkEditor. Tutorials are
 * sequences of steps that highlight panels, show tooltips, display messages,
 * and wait for user actions. Built-in tutorials cover common editor workflows.
 *
 * ## Architecture
 * ```
 * TutorialSystem (singleton)
 *   ├── m_tutorials              (registered tutorial sequences)
 *   ├── m_activeTutorial         (currently running sequence, if any)
 *   ├── m_currentStepIndex       (progress within active tutorial)
 *   ├── m_completedTutorials     (set of finished tutorial names)
 *   └── m_stepChangedCallbacks   (notified when step advances)
 * ```
 *
 * ## Usage
 * @code
 *   auto& tut = SparkEditor::TutorialSystem::GetInstance();
 *   tut.Initialize();
 *
 *   tut.StartTutorial("Getting Started");
 *   // Each frame:
 *   tut.Update(deltaTime);
 *   if (const auto* step = tut.GetCurrentStep())
 *       RenderTutorialOverlay(*step);
 * @endcode
 *
 * @see EditorPanel.h, EditorApplication.h
 */

#pragma once

#include <cstdint>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace SparkEditor
{

    // =========================================================================
    // Enums
    // =========================================================================

    /** @brief The kind of action a tutorial step performs. */
    enum class TutorialStepType : uint8_t
    {
        HighlightPanel, ///< Highlight a named editor panel.
        ShowTooltip,    ///< Show a tooltip near a target panel.
        WaitForAction,  ///< Block until the user performs a specific action.
        ShowMessage,    ///< Display a message dialog.
        WaitForInput    ///< Block until any input is received.
    };

    /** @brief Position of a tooltip relative to its target panel. */
    enum class TooltipPosition : uint8_t
    {
        Top,
        Bottom,
        Left,
        Right
    };

    /** @brief Difficulty classification for a tutorial sequence. */
    enum class TutorialDifficulty : uint8_t
    {
        Beginner,
        Intermediate,
        Advanced
    };

    // =========================================================================
    // Data Structures
    // =========================================================================

    /**
     * @brief A single step within a tutorial sequence.
     */
    struct TutorialStep
    {
        TutorialStepType type = TutorialStepType::ShowMessage;     ///< What this step does.
        std::string targetPanel;                                   ///< Panel name for highlight/tooltip steps.
        std::string message;                                       ///< Instruction text shown to the user.
        TooltipPosition tooltipPosition = TooltipPosition::Bottom; ///< Tooltip placement.
        std::string requiredAction;   ///< Action string the user must perform (for WaitForAction).
        float autoAdvanceDelay = 0.f; ///< Seconds before auto-advancing (0 = manual only).
    };

    /**
     * @brief A named, ordered sequence of tutorial steps.
     */
    struct TutorialSequence
    {
        std::string name;                                             ///< Unique tutorial name.
        std::string description;                                      ///< Short description shown in the tutorial list.
        std::vector<TutorialStep> steps;                              ///< Ordered steps.
        TutorialDifficulty difficulty = TutorialDifficulty::Beginner; ///< Difficulty tag.
    };

    // =========================================================================
    // TutorialSystem
    // =========================================================================

    /**
     * @class TutorialSystem
     * @brief Singleton that manages interactive editor tutorials.
     *
     * Register tutorial sequences, start/stop them, and call Update() each
     * frame to handle auto-advance timers. Callbacks notify the UI layer
     * when the active step changes so overlays can be updated.
     */
    class TutorialSystem
    {
      public:
        /** @brief Get the singleton instance. */
        static TutorialSystem& GetInstance()
        {
            static TutorialSystem s;
            return s;
        }

        // -----------------------------------------------------------------
        // Lifecycle
        // -----------------------------------------------------------------

        /** @brief Initialize the tutorial system and register built-in tutorials. */
        void Initialize()
        {
            m_tutorials.clear();
            m_completedTutorials.clear();
            m_activeTutorial = nullptr;
            m_currentStepIndex = 0;
            m_autoAdvanceTimer = 0.f;
            RegisterDefaultTutorials();
        }

        /** @brief Shut down and release all state. */
        void Shutdown()
        {
            m_tutorials.clear();
            m_completedTutorials.clear();
            m_stepChangedCallbacks.clear();
            m_activeTutorial = nullptr;
            m_currentStepIndex = 0;
        }

        // -----------------------------------------------------------------
        // Registration
        // -----------------------------------------------------------------

        /**
         * @brief Register a tutorial sequence.
         * @param sequence Tutorial to register (copied into internal storage).
         */
        void RegisterTutorial(TutorialSequence sequence)
        {
            std::string name = sequence.name;
            m_tutorials[name] = std::move(sequence);
        }

        // -----------------------------------------------------------------
        // Playback control
        // -----------------------------------------------------------------

        /**
         * @brief Start a tutorial by name.
         * @param name Tutorial name to start.
         * @return True if the tutorial was found and started.
         */
        bool StartTutorial(std::string_view name)
        {
            std::string key(name);
            auto it = m_tutorials.find(key);
            if (it == m_tutorials.end() || it->second.steps.empty())
            {
                return false;
            }

            m_activeTutorial = &it->second;
            m_currentStepIndex = 0;
            m_autoAdvanceTimer = 0.f;
            NotifyStepChanged();
            return true;
        }

        /** @brief Stop the currently active tutorial. */
        void StopTutorial()
        {
            m_activeTutorial = nullptr;
            m_currentStepIndex = 0;
            m_autoAdvanceTimer = 0.f;
        }

        /** @brief Advance to the next step, completing the tutorial if at the end. */
        void AdvanceStep()
        {
            if (!m_activeTutorial)
            {
                return;
            }

            m_autoAdvanceTimer = 0.f;
            uint32_t nextIndex = m_currentStepIndex + 1;
            if (nextIndex >= static_cast<uint32_t>(m_activeTutorial->steps.size()))
            {
                MarkCompleted(m_activeTutorial->name);
                StopTutorial();
                return;
            }

            m_currentStepIndex = nextIndex;
            NotifyStepChanged();
        }

        /**
         * @brief Jump to a specific step index.
         * @param index Zero-based step index.
         */
        void GoToStep(uint32_t index)
        {
            if (!m_activeTutorial)
            {
                return;
            }
            if (index < static_cast<uint32_t>(m_activeTutorial->steps.size()))
            {
                m_currentStepIndex = index;
                m_autoAdvanceTimer = 0.f;
                NotifyStepChanged();
            }
        }

        // -----------------------------------------------------------------
        // Queries
        // -----------------------------------------------------------------

        /** @brief Get the current step, or nullptr if no tutorial is active. */
        [[nodiscard]] const TutorialStep* GetCurrentStep() const
        {
            if (!m_activeTutorial)
            {
                return nullptr;
            }
            if (m_currentStepIndex < static_cast<uint32_t>(m_activeTutorial->steps.size()))
            {
                return &m_activeTutorial->steps[m_currentStepIndex];
            }
            return nullptr;
        }

        /** @brief Zero-based index of the current step. */
        [[nodiscard]] uint32_t GetCurrentStepIndex() const { return m_currentStepIndex; }

        /** @brief Total number of steps in the active tutorial (0 if none). */
        [[nodiscard]] uint32_t GetTotalSteps() const
        {
            return m_activeTutorial ? static_cast<uint32_t>(m_activeTutorial->steps.size()) : 0;
        }

        /** @brief True if a tutorial is currently running. */
        [[nodiscard]] bool IsTutorialActive() const { return m_activeTutorial != nullptr; }

        /** @brief Name of the active tutorial, or empty if none. */
        [[nodiscard]] std::string_view GetCurrentTutorialName() const
        {
            return m_activeTutorial ? std::string_view(m_activeTutorial->name) : std::string_view{};
        }

        /** @brief List names of all registered tutorials. */
        [[nodiscard]] std::vector<std::string_view> GetAvailableTutorials() const
        {
            std::vector<std::string_view> names;
            names.reserve(m_tutorials.size());
            for (const auto& [name, seq] : m_tutorials)
            {
                names.push_back(name);
            }
            return names;
        }

        /**
         * @brief Check if a tutorial has been completed.
         * @param name Tutorial name to check.
         * @return True if the tutorial was completed at least once.
         */
        [[nodiscard]] bool IsTutorialCompleted(std::string_view name) const
        {
            return m_completedTutorials.contains(std::string(name));
        }

        /**
         * @brief Mark a tutorial as completed.
         * @param name Tutorial name to mark.
         */
        void MarkCompleted(std::string_view name) { m_completedTutorials.insert(std::string(name)); }

        // -----------------------------------------------------------------
        // Per-frame update
        // -----------------------------------------------------------------

        /**
         * @brief Per-frame update — handles auto-advance timers.
         * @param dt Delta time in seconds.
         */
        void Update(float dt)
        {
            if (!m_activeTutorial)
            {
                return;
            }

            const TutorialStep* step = GetCurrentStep();
            if (!step || step->autoAdvanceDelay <= 0.f)
            {
                return;
            }

            m_autoAdvanceTimer += dt;
            if (m_autoAdvanceTimer >= step->autoAdvanceDelay)
            {
                AdvanceStep();
            }
        }

        // -----------------------------------------------------------------
        // Callbacks
        // -----------------------------------------------------------------

        /**
         * @brief Register a callback invoked when the active step changes.
         * @param callback Function receiving (stepIndex, step).
         */
        void OnStepChanged(std::function<void(uint32_t, const TutorialStep&)> callback)
        {
            if (callback)
            {
                m_stepChangedCallbacks.push_back(std::move(callback));
            }
        }

        // -----------------------------------------------------------------
        // Console
        // -----------------------------------------------------------------

        /** @brief Human-readable status for the engine console. */
        [[nodiscard]] std::string Console_GetStatus() const
        {
            std::ostringstream oss;
            oss << "[TutorialSystem] tutorials=" << m_tutorials.size() << ", completed=" << m_completedTutorials.size()
                << ", active=" << (m_activeTutorial ? m_activeTutorial->name : "none");
            if (m_activeTutorial)
            {
                oss << ", step=" << (m_currentStepIndex + 1) << "/" << m_activeTutorial->steps.size();
            }
            return oss.str();
        }

      private:
        TutorialSystem() = default;
        TutorialSystem(const TutorialSystem&) = delete;
        TutorialSystem& operator=(const TutorialSystem&) = delete;

        /** @brief Notify all step-changed callbacks. */
        void NotifyStepChanged()
        {
            const TutorialStep* step = GetCurrentStep();
            if (!step)
            {
                return;
            }
            for (const auto& cb : m_stepChangedCallbacks)
            {
                cb(m_currentStepIndex, *step);
            }
        }

        /** @brief Register the built-in default tutorials. */
        void RegisterDefaultTutorials()
        {
            // --- Getting Started ---
            {
                TutorialSequence seq;
                seq.name = "Getting Started";
                seq.description = "Learn the basics of the SparkEditor interface.";
                seq.difficulty = TutorialDifficulty::Beginner;

                seq.steps.push_back({TutorialStepType::ShowMessage,
                                     {},
                                     "Welcome to SparkEditor! This tutorial will walk you through "
                                     "the main interface.",
                                     TooltipPosition::Bottom,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::HighlightPanel,
                                     "Viewport",
                                     "This is the 3D Viewport where you see your scene.",
                                     TooltipPosition::Bottom,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::HighlightPanel,
                                     "SceneHierarchy",
                                     "The Scene Hierarchy lists every entity in your scene.",
                                     TooltipPosition::Right,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::HighlightPanel,
                                     "Inspector",
                                     "Select an entity to view its components here.",
                                     TooltipPosition::Left,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::ShowMessage,
                                     {},
                                     "You are ready to start building! Explore the menus above.",
                                     TooltipPosition::Bottom,
                                     {},
                                     0.f});

                RegisterTutorial(std::move(seq));
            }

            // --- Placing Entities ---
            {
                TutorialSequence seq;
                seq.name = "Placing Entities";
                seq.description = "Learn how to create and position entities in the scene.";
                seq.difficulty = TutorialDifficulty::Beginner;

                seq.steps.push_back({TutorialStepType::ShowMessage,
                                     {},
                                     "In this tutorial you will learn to place entities.",
                                     TooltipPosition::Bottom,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::HighlightPanel,
                                     "SceneHierarchy",
                                     "Right-click here to create a new entity.",
                                     TooltipPosition::Right,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::WaitForAction, "SceneHierarchy", "Create a new entity now.",
                                     TooltipPosition::Right, "CreateEntity", 0.f});

                seq.steps.push_back({TutorialStepType::HighlightPanel,
                                     "Inspector",
                                     "Modify the Transform component to position your entity.",
                                     TooltipPosition::Left,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::ShowMessage,
                                     {},
                                     "Great! You can also drag entities in the viewport.",
                                     TooltipPosition::Bottom,
                                     {},
                                     0.f});

                RegisterTutorial(std::move(seq));
            }

            // --- Material Setup ---
            {
                TutorialSequence seq;
                seq.name = "Material Setup";
                seq.description = "Learn how to create and assign PBR materials.";
                seq.difficulty = TutorialDifficulty::Intermediate;

                seq.steps.push_back({TutorialStepType::ShowMessage,
                                     {},
                                     "This tutorial covers creating and assigning materials.",
                                     TooltipPosition::Bottom,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::HighlightPanel,
                                     "AssetBrowser",
                                     "Open the Asset Browser to find or create materials.",
                                     TooltipPosition::Top,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::WaitForAction, "AssetBrowser",
                                     "Right-click and select 'New Material'.", TooltipPosition::Top, "CreateMaterial",
                                     0.f});

                seq.steps.push_back({TutorialStepType::HighlightPanel,
                                     "MaterialEditor",
                                     "Configure albedo, roughness, metallic, and normal maps here.",
                                     TooltipPosition::Left,
                                     {},
                                     0.f});

                seq.steps.push_back({TutorialStepType::ShowMessage,
                                     {},
                                     "Drag the material onto a mesh in the viewport to apply it.",
                                     TooltipPosition::Bottom,
                                     {},
                                     0.f});

                RegisterTutorial(std::move(seq));
            }
        }

        // -----------------------------------------------------------------
        // State
        // -----------------------------------------------------------------

        std::unordered_map<std::string, TutorialSequence> m_tutorials; ///< Name -> sequence.
        std::set<std::string> m_completedTutorials;                    ///< Names of completed tutorials.
        const TutorialSequence* m_activeTutorial = nullptr;            ///< Currently running tutorial.
        uint32_t m_currentStepIndex = 0;                               ///< Current step within active tutorial.
        float m_autoAdvanceTimer = 0.f;                                ///< Timer for auto-advance steps.
        std::vector<std::function<void(uint32_t, const TutorialStep&)>> m_stepChangedCallbacks;
    };

} // namespace SparkEditor
