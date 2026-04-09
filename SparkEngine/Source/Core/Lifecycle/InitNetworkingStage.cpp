#include "Core/Lifecycle/LifecycleStages.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"

namespace Spark::Core::Lifecycle
{
    class InitNetworkingStage final : public LifecycleStage
    {
      public:
        std::string_view Name() const override { return "InitNetworking"; }
        LifecycleOrder Order() const override { return LifecycleOrder::AI; }
        LifecycleThreadAffinity ThreadAffinity() const override { return LifecycleThreadAffinity::MainThread; }
        bool SupportsInitialize() const override { return true; }

        void Initialize() override { InitializeNetworkingSystemsImpl(); }
    };

    std::unique_ptr<LifecycleStage> CreateInitNetworkingStage()
    {
        return std::make_unique<InitNetworkingStage>();
    }
} // namespace Spark::Core::Lifecycle
