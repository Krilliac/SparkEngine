/**
 * @file GameModule.h
 * @brief {{PROJECT_NAME}} game module
 *
 * This is your game's main module. It implements Spark::IModule and serves
 * as the entry point for all game logic. The engine loads this module at
 * runtime and drives it through the IModule lifecycle.
 */

#pragma once

#include <Spark/SparkSDK.h>

class {{PROJECT_NAME}}Module : public Spark::IModule
{
public:
    {{PROJECT_NAME}}Module() = default;
    ~{{PROJECT_NAME}}Module() override = default;

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name       = "{{PROJECT_NAME}}";
        info.version    = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder  = 1000;
        return info;
    }

    bool OnLoad(Spark::IEngineContext* context) override
    {
        m_context = context;
        // TODO: Initialize your game systems here
        return true;
    }

    void OnUnload() override
    {
        // TODO: Clean up your game systems here
        m_context = nullptr;
    }

    void OnUpdate(float deltaTime) override
    {
        // TODO: Update your game logic here
        (void)deltaTime;
    }

    void OnRender() override
    {
        // TODO: Render your game here
    }

private:
    Spark::IEngineContext* m_context = nullptr;
};
