#include "Utils/InvalidStateDetector.h"
#include "Utils/SparkConsole.h"

#include <Spark/ModuleDllMain.h>
#include <Spark/ModuleRegistry.h>

#include <cstdint>
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
    constexpr const char* kCodeAddressFile = "SPARK_REGISTRY_FIXTURE_CODE_ADDRESS_FILE";

    /// Internal linkage on purpose: a unique (inline) symbol would stop dlclose unmapping the image.
    /// Volatile so the store that keeps a quarantined instance reachable is not optimized away.
    const void* volatile g_quarantinedInstance = nullptr;

    int RegistryFixtureCodeMarker()
    {
        return 0x5A11;
    }
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

        // Publish the address of code in this image, so a test can check the
        // host kept the image mapped after this OnLoad failed.
        if (const char* addressPath = std::getenv(kCodeAddressFile); addressPath && addressPath[0] != '\0')
        {
            if (FILE* addressFile = std::fopen(addressPath, "wb"))
            {
                const auto address = reinterpret_cast<std::uintptr_t>(&RegistryFixtureCodeMarker);
                std::fprintf(addressFile, "%llu\n", static_cast<unsigned long long>(address));
                std::fclose(addressFile);
            }
        }

        const char* throwOnLoad = std::getenv("SPARK_REGISTRY_FIXTURE_THROW_ON_LOAD");
        if (throwOnLoad && throwOnLoad[0] != '\0')
            throw std::runtime_error("intentional registry fixture OnLoad failure");

        return true;
    }

    void OnUnload() override
    {
        const char* throwOnUnload = std::getenv("SPARK_REGISTRY_FIXTURE_THROW_ON_UNLOAD");
        if (throwOnUnload && throwOnUnload[0] != '\0')
        {
            // The host must quarantine, not destroy, an instance whose partial
            // OnUnload did not finish. Keep it reachable from this (retained)
            // image so leak checkers see the intended quarantine as reachable.
            g_quarantinedInstance = this;
            throw std::runtime_error("intentional registry fixture OnUnload failure");
        }

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
