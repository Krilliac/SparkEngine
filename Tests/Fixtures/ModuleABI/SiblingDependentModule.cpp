#include <Spark/ModuleRegistry.h>

#include <cstdint>

extern "C" uint32_t SparkTestSiblingDependencyValue();

namespace
{
    class SiblingDependentModule final : public Spark::IModule
    {
      public:
        Spark::ModuleInfo GetModuleInfo() const override
        {
            Spark::ModuleInfo info{};
            info.name = "Spark Sibling Dependency Fixture";
            info.version = "1.0.0";
            info.kind = Spark::ModuleKind::Addon;
            return info;
        }

        bool OnLoad(Spark::IEngineContext*) override { return SparkTestSiblingDependencyValue() == 0x51B11A7u; }
        void OnUnload() override {}
        bool CanUnload() override { return true; }
        void OnUpdate(float) override {}
    };
} // namespace

SPARK_IMPLEMENT_MODULE(SiblingDependentModule)
