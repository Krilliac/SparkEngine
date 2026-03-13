// Timer.cpp
#include "Timer.h"
#include "Validate.h"
#include <iostream>

Timer::Timer() : m_deltaTime(0.0f), m_totalTime(0.0f), m_paused(false)
{
    m_lastTime = std::chrono::high_resolution_clock::now();
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Timer constructed");
}

Timer::~Timer()
{
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Timer destructor called");
}

void Timer::Start()
{
    if (m_paused)
    {
        m_lastTime = std::chrono::high_resolution_clock::now();
        m_paused = false;
        SPARK_LOG_INFO(Spark::LogCategory::Core, "Timer started");
    }
}

void Timer::Stop()
{
    m_paused = true;
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Timer stopped");
}

void Timer::Reset()
{
    m_lastTime = std::chrono::high_resolution_clock::now();
    m_deltaTime = 0.0f;
    m_totalTime = 0.0f;
    m_paused = false;
    SPARK_LOG_INFO(Spark::LogCategory::Core, "Timer reset");
}

float Timer::GetDeltaTime()
{
    if (!m_paused)
    {
        UpdateTime();
    }
    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, m_deltaTime >= 0.0f, "Delta time should never be negative");
    return m_deltaTime;
}

void Timer::UpdateTime()
{
    auto currentTime = std::chrono::high_resolution_clock::now();
    std::chrono::duration<float> diff = currentTime - m_lastTime;

    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, diff.count() >= 0.0f, "Time difference must be non-negative");
    m_deltaTime = diff.count();

    // Cap delta time to prevent large jumps
    if (m_deltaTime > 0.05f)
    {
        m_deltaTime = 0.05f;
    }

    m_lastTime += std::chrono::duration_cast<std::chrono::high_resolution_clock::duration>(diff);
    m_totalTime += m_deltaTime;

    SPARK_REQUIRE_MSG(Spark::LogCategory::Core, m_totalTime >= 0.0f, "Total time should never be negative");
}