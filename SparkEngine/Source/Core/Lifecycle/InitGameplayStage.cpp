#include "Core/Lifecycle/LifecycleStages.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"

namespace Spark::Core::Lifecycle
{
    class InitGameplayStage final : public LifecycleStage
    {
      public:
        std::string_view Name() const override { return "InitGameplay"; }
        LifecycleOrder Order() const override { return LifecycleOrder::Audio; }
        LifecycleThreadAffinity ThreadAffinity() const override { return LifecycleThreadAffinity::MainThread; }
        bool SupportsInitialize() const override { return true; }

        void Initialize() override { InitializeGameplaySystemsImpl(); }
    };

    std::unique_ptr<LifecycleStage> CreateInitGameplayStage()
    {
        return std::make_unique<InitGameplayStage>();
    }
} // namespace Spark::Core::Lifecycle
