#include "GameplaySystemLifecycle.h"

#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Core/Lifecycle/LifecycleCompositionRoot.h"
#include "Core/Lifecycle/LifecycleStages.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Spark::Core::Lifecycle
{
    namespace
    {
        constexpr std::array<RequiredStage, 5> kProductionRequiredStages = {
            RequiredStage{"InitNetworking", LifecyclePhase::Initialize},
            RequiredStage{"InitGameplay", LifecyclePhase::Initialize},
            RequiredStage{"InitDebug", LifecyclePhase::Initialize},
            RequiredStage{"Update", LifecyclePhase::Update},
            RequiredStage{"Shutdown", LifecyclePhase::Shutdown},
        };

        std::vector<std::unique_ptr<LifecycleStage>> CreateProductionStages()
        {
            std::vector<std::unique_ptr<LifecycleStage>> stages;
            stages.emplace_back(CreateInitNetworkingStage());
            stages.emplace_back(CreateInitGameplayStage());
            stages.emplace_back(CreateInitDebugStage());
            stages.emplace_back(CreateUpdateStage());
            stages.emplace_back(CreateShutdownStage());
            return stages;
        }

        bool StageSupports(const LifecycleStage& stage, LifecyclePhase phase)
        {
            switch (phase)
            {
            case LifecyclePhase::Initialize:
                return stage.SupportsInitialize();
            case LifecyclePhase::Update:
                return stage.SupportsUpdate();
            case LifecyclePhase::Shutdown:
                return stage.SupportsShutdown();
            }
            return false;
        }

        /// Runs one stage's Shutdown with exceptions contained, so one failing
        /// teardown never strands the stages after it.
        bool ShutdownStageContained(LifecycleStage& stage)
        {
            auto& console = Spark::SimpleConsole::GetInstance();
            try
            {
                stage.Shutdown();
                return true;
            }
            catch (const std::exception& exception)
            {
                console.LogError(
                    std::format("[Lifecycle] Stage '{}' threw during shutdown: {}", stage.Name(), exception.what()));
            }
            catch (...)
            {
                console.LogError(
                    std::format("[Lifecycle] Stage '{}' threw an unknown exception during shutdown", stage.Name()));
            }
            return false;
        }
    } // namespace

    LifecycleCompositionRoot& LifecycleCompositionRoot::Get()
    {
        static LifecycleCompositionRoot instance(CreateProductionStages(), kProductionRequiredStages);
        return instance;
    }

    LifecycleCompositionRoot::LifecycleCompositionRoot(std::vector<std::unique_ptr<LifecycleStage>> stages,
                                                       std::span<const RequiredStage> requiredStages)
        : m_stages(std::move(stages))
    {
        const bool hasNullStage =
            std::any_of(m_stages.begin(), m_stages.end(), [](const auto& stage) { return stage == nullptr; });
        if (hasNullStage)
        {
            Spark::SimpleConsole::GetInstance().LogError(
                "[LifecycleValidation] Null lifecycle stage; lifecycle execution disabled.");
            m_stages.clear();
            m_state = LifecycleRootState::Failed;
            return;
        }

        std::stable_sort(m_stages.begin(), m_stages.end(),
                         [](const std::unique_ptr<LifecycleStage>& lhs, const std::unique_ptr<LifecycleStage>& rhs)
                         {
                             if (lhs->Order() != rhs->Order())
                                 return lhs->Order() < rhs->Order();
                             return lhs->Name() < rhs->Name();
                         });

        m_initializeAttempted.assign(m_stages.size(), false);
        m_isValid = ValidateConfiguration(requiredStages);
        if (!m_isValid)
            m_state = LifecycleRootState::Failed;
    }

    bool LifecycleCompositionRoot::RunInitialize()
    {
        if (m_state == LifecycleRootState::Initialized)
            return true;

        auto& console = Spark::SimpleConsole::GetInstance();
        if (m_state != LifecycleRootState::Idle && m_state != LifecycleRootState::ShutDown)
        {
            // Failed is latched (invalid composition or an earlier failure);
            // Initializing/ShuttingDown means a stage re-entered the root.
            console.LogError("[Lifecycle] Initialize rejected: lifecycle is failed or mid-transition.");
            return false;
        }

        m_state = LifecycleRootState::Initializing;
        for (size_t index = 0; index < m_stages.size(); ++index)
        {
            LifecycleStage& stage = *m_stages[index];
            if (!stage.SupportsInitialize())
                continue;

            m_initializeAttempted[index] = true;
            if (InitializeStage(stage))
                continue;

            console.LogError(std::format(
                "[Lifecycle] Stage '{}' failed to initialize; rolling back initialized stages.", stage.Name()));
            if (!RunTeardown())
                console.LogError("[Lifecycle] Rollback after failed initialization was not clean.");
            m_state = LifecycleRootState::Failed;
            return false;
        }

        m_state = LifecycleRootState::Initialized;
        return true;
    }

    void LifecycleCompositionRoot::RunUpdate(float dt)
    {
        // Idle still updates: -minimal-init skips RunInitialize but keeps the
        // per-frame pump, and every production update is guarded.
        if (m_state != LifecycleRootState::Idle && m_state != LifecycleRootState::Initialized)
            return;

        for (const auto& stage : m_stages)
        {
            if (stage->SupportsUpdate())
                stage->Update(dt);
        }
    }

    bool LifecycleCompositionRoot::RunShutdown()
    {
        switch (m_state)
        {
        case LifecycleRootState::ShutDown:
            return true;
        case LifecycleRootState::Failed:
            // Either rollback already tore everything down, or a previous
            // teardown failed; running stages again would double-shutdown.
            return false;
        case LifecycleRootState::Initializing:
        case LifecycleRootState::ShuttingDown:
            Spark::SimpleConsole::GetInstance().LogError("[Lifecycle] Shutdown rejected: lifecycle is mid-transition.");
            return false;
        case LifecycleRootState::Idle:
        case LifecycleRootState::Initialized:
            break;
        }

        m_state = LifecycleRootState::ShuttingDown;
        const bool clean = RunTeardown();
        m_state = clean ? LifecycleRootState::ShutDown : LifecycleRootState::Failed;
        return clean;
    }

    bool LifecycleCompositionRoot::InitializeStage(LifecycleStage& stage)
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        try
        {
            return stage.Initialize();
        }
        catch (const std::exception& exception)
        {
            console.LogError(
                std::format("[Lifecycle] Stage '{}' threw during initialize: {}", stage.Name(), exception.what()));
        }
        catch (...)
        {
            console.LogError(
                std::format("[Lifecycle] Stage '{}' threw an unknown exception during initialize", stage.Name()));
        }
        return false;
    }

    bool LifecycleCompositionRoot::RunTeardown()
    {
        bool clean = true;
        for (size_t index = m_stages.size(); index-- > 0;)
        {
            LifecycleStage& stage = *m_stages[index];
            const bool ownsTeardown = !stage.SupportsInitialize() || m_initializeAttempted[index];
            // Clear before calling out so a re-entrant path cannot shut a stage down twice.
            m_initializeAttempted[index] = false;
            if (stage.SupportsShutdown() && ownsTeardown)
                clean = ShutdownStageContained(stage) && clean;
        }
        return clean;
    }

    bool LifecycleCompositionRoot::ValidateConfiguration(std::span<const RequiredStage> requiredStages) const
    {
        constexpr std::array<LifecycleOrder, 6> kEcsExecutionOrder = {
            LifecycleOrder::Physics, LifecycleOrder::Animation, LifecycleOrder::AI,
            LifecycleOrder::Audio,   LifecycleOrder::Lifecycle, LifecycleOrder::Render,
        };

        auto& console = Spark::SimpleConsole::GetInstance();
        for (size_t i = 1; i < kEcsExecutionOrder.size(); ++i)
        {
            if (kEcsExecutionOrder[i - 1] >= kEcsExecutionOrder[i])
            {
                console.LogError("[LifecycleValidation] ECS order metadata is not strictly increasing.");
                return false;
            }
        }

        bool valid = true;
        for (const RequiredStage& required : requiredStages)
        {
            const bool matched =
                std::any_of(m_stages.begin(), m_stages.end(), [&](const std::unique_ptr<LifecycleStage>& stage)
                            { return stage->Name() == required.name && StageSupports(*stage, required.phase); });
            if (!matched)
            {
                valid = false;
                console.LogError(std::string("[LifecycleValidation] Missing/wired stage: ") +
                                 std::string(required.name) + " for requested phase.");
            }
        }

        if (!valid)
        {
            console.LogError(
                "[LifecycleValidation] Lifecycle stage composition invalid; lifecycle execution disabled.");
        }

        return valid;
    }
} // namespace Spark::Core::Lifecycle

namespace
{
    using Spark::Core::Lifecycle::LifecycleCompositionRoot;
} // namespace

void LogMissingModuleWarnings()
{
    Spark::Core::Lifecycle::LogMissingModuleWarningsImpl();
}

bool InitDebugSystems()
{
    return LifecycleCompositionRoot::Get().RunInitialize();
}

void InitGameplaySystems()
{
    // LifecycleCompositionRoot runs full initialize sequence in deterministic stage order.
    // Retained for API compatibility with existing SparkEngine call sites.
}

void UpdateGameplaySystems(float dt)
{
    LifecycleCompositionRoot::Get().RunUpdate(dt);
}

void UpdateDebugSystems(float /*dt*/)
{
    // Debug update runs in UpdateStage to preserve deterministic scheduling.
}

bool ShutdownGameplaySystems()
{
    return LifecycleCompositionRoot::Get().RunShutdown();
}

void ShutdownDebugSystems()
{
    // Debug shutdown runs in ShutdownStage to preserve deterministic scheduling.
}

uint64_t GetGameplayFrameCount()
{
    return Spark::Core::Lifecycle::GetGameplayFrameCountImpl();
}
