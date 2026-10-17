/**
 * @file EngineContext.cpp
 * @brief Concrete IEngineContext implementation
 *
 * R1.1: Constructor now delegates to RegisterSystem<T> for each subsystem.
 */

#include "EngineContext.h"
#include "Spark/Version.h"

#include <memory>

// Global engine context - defined here (in SparkEngineLib) so that all
// consumers of the static library (both the executable and SparkGame DLL)
// can resolve this symbol at link time.
std::unique_ptr<EngineContext> g_engineContext;

// Non-owning pointer to a host EngineContext injected across a DLL boundary.
// SparkEngineLib is statically linked into every module DLL, so g_engineContext
// is a per-image global that is null inside modules. When the host injects its
// live context via SetInjected(), Get() prefers it so module-side EngineContext::Get()
// resolves to the engine's real context instead of a dead per-image instance.
// It is deliberately NOT a unique_ptr — modules must never own/free the host context.
static EngineContext* g_injectedContext = nullptr;

EngineContext* EngineContext::Get()
{
    if (g_injectedContext)
    {
        return g_injectedContext;
    }
    return g_engineContext.get();
}

void EngineContext::SetInjected(EngineContext* ctx)
{
    g_injectedContext = ctx;
}

const std::unique_ptr<EngineContext>& EngineContext::GetOwned()
{
    return g_engineContext;
}

void EngineContext::SetOwned(std::unique_ptr<EngineContext> ctx)
{
    g_engineContext = std::move(ctx);
}

void EngineContext::ResetOwned()
{
    g_engineContext.reset();
}

EngineContext::EngineContext(GraphicsEngine* graphics, InputManager* input, Timer* timer, Spark::EventBus* eventBus)
{
    // Delegate all storage to the generic registry (R1.1)
    if (graphics)
    {
        RegisterSystem<GraphicsEngine>(graphics);
    }
    if (input)
    {
        RegisterSystem<InputManager>(input);
    }
    if (timer)
    {
        RegisterSystem<Timer>(timer);
    }
    if (eventBus)
    {
        RegisterSystem<Spark::EventBus>(eventBus);
    }
}

uint32_t EngineContext::GetEngineVersion() const
{
    return Spark::GetEngineVersion();
}

uint32_t EngineContext::GetSDKVersion() const
{
    return Spark::GetSDKVersion();
}

#ifdef SPARK_HEADLESS_SUPPORT
// NOTE: g_headlessMode is a plain bool because it is also referenced via
// `extern bool g_headlessMode;` from SparkEngineWindows.cpp / SparkEngineLinux.cpp
// (which write it) and ModuleManager.cpp (which reads it). Promoting it to
// std::atomic<bool> here without updating those extern declarations in lockstep
// would be an ODR type mismatch. In practice it is written once at startup
// (command-line parse) before worker threads spawn, so the read is effectively
// safe; a proper fix (atomic + coordinated extern-decl update, or folding the
// flag into EngineRuntime state) must touch those out-of-lane TUs together.
bool g_headlessMode = false;
#endif

bool EngineContext::IsHeadless() const
{
#ifdef SPARK_HEADLESS_SUPPORT
    return g_headlessMode;
#else
    return false;
#endif
}
