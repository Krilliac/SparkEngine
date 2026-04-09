#include "GameplaySystemLifecycle.h"

#include "Core/Lifecycle/GameplayLifecycleShared.h"
#include "Core/Lifecycle/LifecycleStages.h"
#include "Utils/SparkConsole.h"

#include <algorithm>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    using Spark::Core::Lifecycle::CreateInitDebugStage;
    using Spark::Core::Lifecycle::CreateInitGameplayStage;
    using Spark::Core::Lifecycle::CreateInitNetworkingStage;
    using Spark::Core::Lifecycle::CreateShutdownStage;
    using Spark::Core::Lifecycle::CreateUpdateStage;
    using Spark::Core::Lifecycle::LifecycleOrder;
    using Spark::Core::Lifecycle::LifecycleStage;

    enum class LifecyclePhase : uint8_t
    {
        Initialize,
        Update,
        Shutdown
    };

    struct RequiredStage
    {
        std::string_view name;
        LifecyclePhase phase;
    };

    class LifecycleCompositionRoot
    {
      public:
        static LifecycleCompositionRoot& Get()
        {
            static LifecycleCompositionRoot instance;
            return instance;
        }

        void RunInitialize() { ExecutePhase(LifecyclePhase::Initialize, 0.0f); }

        void RunUpdate(float dt) { ExecutePhase(LifecyclePhase::Update, dt); }

        void RunShutdown() { ExecutePhase(LifecyclePhase::Shutdown, 0.0f); }

      private:
        LifecycleCompositionRoot()
        {
            m_stages.emplace_back(CreateInitNetworkingStage());
            m_stages.emplace_back(CreateInitGameplayStage());
            m_stages.emplace_back(CreateInitDebugStage());
            m_stages.emplace_back(CreateUpdateStage());
            m_stages.emplace_back(CreateShutdownStage());

            std::sort(m_stages.begin(), m_stages.end(),
                      [](const std::unique_ptr<LifecycleStage>& lhs, const std::unique_ptr<LifecycleStage>& rhs)
                      {
                          if (lhs->Order() != rhs->Order())
                              return lhs->Order() < rhs->Order();
                          return lhs->Name() < rhs->Name();
                      });

            m_isValid = ValidateConfiguration();
        }

        void ExecutePhase(LifecyclePhase phase, float dt)
        {
            if (!m_isValid)
                return;

            for (const auto& stage : m_stages)
            {
                switch (phase)
                {
                case LifecyclePhase::Initialize:
                    if (stage->SupportsInitialize())
                        stage->Initialize();
                    break;
                case LifecyclePhase::Update:
                    if (stage->SupportsUpdate())
                        stage->Update(dt);
                    break;
                case LifecyclePhase::Shutdown:
                    if (stage->SupportsShutdown())
                        stage->Shutdown();
                    break;
                }
            }
        }

        bool ValidateConfiguration()
        {
            constexpr std::array<RequiredStage, 5> kRequiredStages = {
                RequiredStage{"InitNetworking", LifecyclePhase::Initialize},
                RequiredStage{"InitGameplay", LifecyclePhase::Initialize},
                RequiredStage{"InitDebug", LifecyclePhase::Initialize},
                RequiredStage{"Update", LifecyclePhase::Update},
                RequiredStage{"Shutdown", LifecyclePhase::Shutdown},
            };

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

            for (const RequiredStage& required : kRequiredStages)
            {
                bool matched = false;
                for (const auto& stage : m_stages)
                {
                    if (stage->Name() != required.name)
                        continue;

                    matched = (required.phase == LifecyclePhase::Initialize && stage->SupportsInitialize()) ||
                              (required.phase == LifecyclePhase::Update && stage->SupportsUpdate()) ||
                              (required.phase == LifecyclePhase::Shutdown && stage->SupportsShutdown());
                    if (matched)
                        break;
                }

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

        std::vector<std::unique_ptr<LifecycleStage>> m_stages;
        bool m_isValid = false;
    };
} // namespace

void LogMissingModuleWarnings()
{
    Spark::Core::Lifecycle::LogMissingModuleWarningsImpl();
}

void InitDebugSystems()
{
    LifecycleCompositionRoot::Get().RunInitialize();
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

void ShutdownGameplaySystems()
{
    LifecycleCompositionRoot::Get().RunShutdown();
}

void ShutdownDebugSystems()
{
    // Debug shutdown runs in ShutdownStage to preserve deterministic scheduling.
}

uint64_t GetGameplayFrameCount()
{
    return Spark::Core::Lifecycle::GetGameplayFrameCountImpl();
}
