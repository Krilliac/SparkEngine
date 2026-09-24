#pragma once

#include "Game/FPSWeatherPort.h"

#include <thread>

namespace SparkGameFPS
{
    /// Owns FPS weather call policy while borrowing the Core-bound port.
    /// All methods are game-thread-only; wrong-thread calls are rejected.
    class FPSWeatherIntegration final
    {
      public:
        FPSWeatherIntegration() : m_gameThread(std::this_thread::get_id()) {}

        void Bind(IFPSWeatherPort* port)
        {
            if (OnGameThread())
                m_port = port;
        }

        bool Initialize()
        {
            if (!OnGameThread() || m_initialized)
                return m_active;

            m_initialized = true;
            m_active = m_port && m_port->SetWeather(WeatherPreset::Clear, 1.0f, 0.0f);
            return m_active;
        }

        bool SetWeather(WeatherPreset preset, float intensity, float transitionSeconds)
        {
            if (!OnGameThread() || !m_port)
                return false;
            return m_port->SetWeather(preset, intensity, transitionSeconds);
        }

        void Clear()
        {
            if (!OnGameThread())
                return;
            m_port = nullptr;
            m_active = false;
            m_initialized = false;
        }

        bool IsActive() const { return m_active; }

      private:
        bool OnGameThread() const { return std::this_thread::get_id() == m_gameThread; }

        IFPSWeatherPort* m_port{nullptr};
        std::thread::id m_gameThread;
        bool m_initialized{false};
        bool m_active{false};
    };
} // namespace SparkGameFPS
