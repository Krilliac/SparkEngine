#include "Core/Lifecycle/LifecycleStages.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"

namespace Spark::Core::Lifecycle
{
    class InitDebugStage final : public LifecycleStage
    {
      public:
        std::string_view Name() const override { return "InitDebug"; }
        LifecycleOrder Order() const override { return LifecycleOrder::Diagnostics; }
        LifecycleThreadAffinity ThreadAffinity() const override { return LifecycleThreadAffinity::MainThread; }
        bool SupportsInitialize() const override { return true; }

        void Initialize() override { InitializeDebugSystemsImpl(); }
    };

    std::unique_ptr<LifecycleStage> CreateInitDebugStage()
    {
        return std::make_unique<InitDebugStage>();
    }
} // namespace Spark::Core::Lifecycle
