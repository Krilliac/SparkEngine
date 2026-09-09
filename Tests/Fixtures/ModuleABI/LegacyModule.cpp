#include "Core/IGameModule.h"
#include <Spark/ModuleABI.h>

#include <cstdlib>

namespace
{
    class LegacyModuleFixture final : public IGameModule
    {
      public:
        const char* GetGameName() const override { return "Spark Legacy Adapter Fixture"; }
        const char* GetGameVersion() const override { return "1.0.0"; }

        bool Initialize(GraphicsEngine*, InputManager*) override
        {
            const char* failOnLoad = std::getenv("SPARK_LEGACY_MODULE_FAIL_ON_LOAD");
            return !failOnLoad || failOnLoad[0] == '\0';
        }

        void Shutdown() override {}
        void Update(float) override {}
    };
}

SPARK_EXPORT_MODULE_COMPATIBILITY()

extern "C"
{
    SPARK_MODULE_API IGameModule* CreateGameModule()
    {
        return new LegacyModuleFixture();
    }

    SPARK_MODULE_API void DestroyGameModule(IGameModule* module)
    {
        delete module;
    }
}
