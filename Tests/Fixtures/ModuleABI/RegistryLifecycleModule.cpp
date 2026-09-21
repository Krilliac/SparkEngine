#include "Utils/InvalidStateDetector.h"
#include "Utils/SparkConsole.h"

#include <Spark/ModuleDllMain.h>
#include <Spark/ModuleRegistry.h>

#include <cstdlib>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kCommandName = "registry_fixture_status";
    constexpr const char* kRuleName = "RegistryFixture.Rule";
    constexpr const char* kRuleCategory = "RegistryFixture";
    constexpr const char* kLifecycleSentinel = "SPARK_REGISTRY_FIXTURE_LIFECYCLE_SENTINEL";
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

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        auto& console = Spark::SimpleConsole::GetInstance();
        console.RegisterCommand(
            kCommandName, [](const std::vector<std::string>&) { return std::string{"fixture"}; },
            "Registry lifecycle fixture command", "Tests");

        Spark::InvalidStateDetector::GetInstance().AddRule({kRuleName, kRuleCategory,
                                                            Spark::StateViolationSeverity::Warning, true,
                                                            [](World&, std::vector<Spark::StateViolation>&) {}});

        const char* throwOnLoad = std::getenv("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD");
        if (throwOnLoad && throwOnLoad[0] != '\0')
            throw std::runtime_error("intentional registry fixture OnLoad failure");

        return true;
    }

    void OnUnload() override
    {
        if (const char* sentinelPath = std::getenv(kLifecycleSentinel); sentinelPath && sentinelPath[0] != '\0')
        {
            if (FILE* sentinel = std::fopen(sentinelPath, "wb"))
            {
                const bool servicesAlive = m_context && m_context->GetWeather() && m_context->GetUI() &&
                                            m_context->GetDialogue() && m_context->GetModSystem();
                std::fputs(servicesAlive ? "services_alive\n" : "services_missing\n", sentinel);
                std::fclose(sentinel);
            }
        }
        Spark::SimpleConsole::GetInstance().UnregisterCommand(kCommandName);
        Spark::InvalidStateDetector::GetInstance().RemoveRulesByCategory(kRuleCategory);
        m_context = nullptr;
    }

    void OnUpdate(float) override {}

  private:
    Spark::IEngineContext* m_context = nullptr;
};

SPARK_IMPLEMENT_MODULE(RegistryLifecycleModule)
