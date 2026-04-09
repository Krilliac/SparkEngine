#include "Core/Lifecycle/LifecycleStages.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"

namespace Spark::Core::Lifecycle
{
    class ShutdownStage final : public LifecycleStage
    {
      public:
        std::string_view Name() const override { return "Shutdown"; }
        LifecycleOrder Order() const override { return LifecycleOrder::Lifecycle; }
        LifecycleThreadAffinity ThreadAffinity() const override { return LifecycleThreadAffinity::MainThread; }
        bool SupportsShutdown() const override { return true; }

        void Shutdown() override
        {
            ShutdownGameplaySystemsImpl();
            ShutdownDebugSystemsImpl();
        }
    };

    std::unique_ptr<LifecycleStage> CreateShutdownStage()
    {
        return std::make_unique<ShutdownStage>();
    }
} // namespace Spark::Core::Lifecycle
