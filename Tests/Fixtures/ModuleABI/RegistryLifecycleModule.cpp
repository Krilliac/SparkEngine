#include "Utils/InvalidStateDetector.h"
#include "Utils/SparkConsole.h"

#include <Spark/ModuleDllMain.h>
#include <Spark/ModuleRegistry.h>

#include <string>
#include <vector>

namespace
{
    constexpr const char* kCommandName = "registry_fixture_status";
    constexpr const char* kRuleName = "RegistryFixture.Rule";
    constexpr const char* kRuleCategory = "RegistryFixture";
} // namespace

class RegistryLifecycleModule final : public Spark::IModule
{
  public:
    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "Spark Registry Lifecycle Fixture";
        info.version = "1.0.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext*) override
    {
        auto& console = Spark::SimpleConsole::GetInstance();
        console.RegisterCommand(
            kCommandName, [](const std::vector<std::string>&) { return std::string{"fixture"}; },
            "Registry lifecycle fixture command", "Tests");

        Spark::InvalidStateDetector::GetInstance().AddRule({kRuleName, kRuleCategory,
                                                            Spark::StateViolationSeverity::Warning, true,
                                                            [](World&, std::vector<Spark::StateViolation>&) {}});
        return true;
    }

    void OnUnload() override
    {
        Spark::SimpleConsole::GetInstance().UnregisterCommand(kCommandName);
        Spark::InvalidStateDetector::GetInstance().RemoveRulesByCategory(kRuleCategory);
    }

    void OnUpdate(float) override {}
};

SPARK_IMPLEMENT_MODULE(RegistryLifecycleModule)
