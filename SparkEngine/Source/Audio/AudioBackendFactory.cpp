/**
 * @file AudioBackendFactory.cpp
 * @brief Platform-aware factory for audio backend selection
 */

#include "AudioBackendFactory.h"
#include "Core/Platform.h"
#include "NullAudioBackend.h"

#ifdef SPARK_PLATFORM_WINDOWS
#include "XAudio2AudioBackend.h"
#else
#include "OpenALAudioBackend.h"
#endif

#include "Utils/LogMacros.h"

namespace Spark::Audio
{

    std::unique_ptr<IAudioBackend> CreateAudioBackend(AudioBackendType type, AudioEngine* engine)
    {
        // Explicit backend request
        if (type == AudioBackendType::Null)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Audio, "Audio backend: Null (silent)");
            return std::make_unique<NullAudioBackend>();
        }

#ifdef SPARK_PLATFORM_WINDOWS
        if (type == AudioBackendType::Auto || type == AudioBackendType::XAudio2)
        {
            if (engine)
            {
                SPARK_LOG_INFO(Spark::LogCategory::Audio, "Audio backend: XAudio2");
                return std::make_unique<XAudio2AudioBackend>(engine);
            }
            SPARK_LOG_WARN(Spark::LogCategory::Audio,
                           "XAudio2 backend requested but no AudioEngine provided, falling back to Null");
        }
#else
        (void)engine; // Unused on non-Windows

        if (type == AudioBackendType::Auto || type == AudioBackendType::OpenAL)
        {
            SPARK_LOG_INFO(Spark::LogCategory::Audio, "Audio backend: OpenAL Soft");
            return std::make_unique<OpenALAudioBackend>();
        }
#endif

        SPARK_LOG_INFO(Spark::LogCategory::Audio, "Audio backend: Null (no suitable backend found)");
        return std::make_unique<NullAudioBackend>();
    }

    const char* GetBackendName(AudioBackendType type)
    {
        switch (type)
        {
        case AudioBackendType::XAudio2:
            return "XAudio2";
        case AudioBackendType::OpenAL:
            return "OpenAL";
        case AudioBackendType::Null:
            return "Null";
        case AudioBackendType::Auto:
            return "Auto";
        }
        return "Unknown";
    }

} // namespace Spark::Audio
