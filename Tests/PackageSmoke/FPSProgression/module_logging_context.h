/** @file module_logging_context.h
 *  @brief SDK-only host doubles shared by the logging and precondition consumers.
 */
#pragma once

#include <cstddef>

#include <Spark/IConsole.h>
#include <Spark/IEngineContext.h>

class ProbeConsole final : public Spark::IConsole
{
  public:
    bool RegisterCommand(std::string_view, CommandHandler, std::string_view, std::string_view,
                         std::string_view) override
    {
        return false;
    }
    void UnregisterCommand(std::string_view) override {}
    void Print(std::string_view, std::string_view) override { ++m_printCount; }
    [[nodiscard]] std::size_t PrintCount() const { return m_printCount; }

  private:
    std::size_t m_printCount = 0;
};

class ProbeContext final : public Spark::IEngineContext
{
  public:
    explicit ProbeContext(Spark::ILogger* logger) : m_logger(logger) {}

    GraphicsEngine* GetGraphics() override { return nullptr; }
    const GraphicsEngine* GetGraphics() const override { return nullptr; }
    InputManager* GetInput() override { return nullptr; }
    const InputManager* GetInput() const override { return nullptr; }
    Timer* GetTimer() override { return nullptr; }
    const Timer* GetTimer() const override { return nullptr; }
    Spark::EventBus* GetEventBus() override { return nullptr; }
    const Spark::EventBus* GetEventBus() const override { return nullptr; }
    AudioEngine* GetAudio() override { return nullptr; }
    const AudioEngine* GetAudio() const override { return nullptr; }
    PhysicsSystem* GetPhysics() override { return nullptr; }
    const PhysicsSystem* GetPhysics() const override { return nullptr; }
    uint32_t GetEngineVersion() const override { return 0; }
    uint32_t GetSDKVersion() const override { return SPARK_SDK_VERSION; }
    Spark::ILogger* GetLogger() override { return m_logger; }
    Spark::IConsole* GetConsole() override { return &m_console; }
    [[nodiscard]] std::size_t ConsolePrintCount() const { return m_console.PrintCount(); }

  private:
    Spark::ILogger* m_logger;
    ProbeConsole m_console;
};
