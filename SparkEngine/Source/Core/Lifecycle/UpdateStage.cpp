#include "Core/Lifecycle/LifecycleStages.h"
#include "Core/Lifecycle/GameplayLifecycleShared.h"

namespace Spark::Core::Lifecycle
{
    class UpdateStage final : public LifecycleStage
    {
      public:
        std::string_view Name() const override { return "Update"; }
        LifecycleOrder Order() const override { return LifecycleOrder::Lifecycle; }
        LifecycleThreadAffinity ThreadAffinity() const override { return LifecycleThreadAffinity::MainThread; }
        bool SupportsUpdate() const override { return true; }

        void Update(float dt) override
        {
            UpdateGameplaySystemsImpl(dt);
            UpdateDebugSystemsImpl(dt);
        }
    };

    std::unique_ptr<LifecycleStage> CreateUpdateStage()
    {
        return std::make_unique<UpdateStage>();
    }
} // namespace Spark::Core::Lifecycle
