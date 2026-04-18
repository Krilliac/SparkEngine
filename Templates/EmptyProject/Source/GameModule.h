/**
 * @file GameModule.h
 * @brief EmptyProject game module
 *
 * This is your game's main module. It implements Spark::IModule and serves
 * as the entry point for all game logic. The engine loads this module at
 * runtime and drives it through the IModule lifecycle.
 */

#pragma once

#include <Spark/SparkSDK.h>

class EmptyProjectModule : public Spark::IModule
{
  public:
    EmptyProjectModule() = default;
    ~EmptyProjectModule() override = default;

    Spark::ModuleInfo GetModuleInfo() const override
    {
        Spark::ModuleInfo info{};
        info.name = "EmptyProject";
        info.version = "0.1.0";
        info.sdkVersion = SPARK_SDK_VERSION;
        info.loadOrder = 1000;
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
