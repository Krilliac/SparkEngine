#pragma once

#include <Spark/SparkSDK.h>

#include <cstdint>

class EmptyProjectModule final : public Spark::IModule
{
  public:
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
        m_updateCount = 0;
        m_elapsedSeconds = 0.0f;
        m_paused = false;
        return true;
    }

    void OnUnload() override
    {
        m_context = nullptr;
        m_paused = false;
    }

    void OnUpdate(float deltaTime) override
    {
        if (m_paused || deltaTime <= 0.0f)
            return;
        ++m_updateCount;
        m_elapsedSeconds += deltaTime;
    }

    void OnPause() override { m_paused = true; }
    void OnResume() override { m_paused = false; }

    [[nodiscard]] uint64_t GetUpdateCount() const { return m_updateCount; }
    [[nodiscard]] float GetElapsedSeconds() const { return m_elapsedSeconds; }
    [[nodiscard]] bool IsPaused() const { return m_paused; }
    [[nodiscard]] bool HasEngineContext() const { return m_context != nullptr; }

  private:
    Spark::IEngineContext* m_context = nullptr;
    uint64_t m_updateCount = 0;
    float m_elapsedSeconds = 0.0f;
    bool m_paused = false;
};
