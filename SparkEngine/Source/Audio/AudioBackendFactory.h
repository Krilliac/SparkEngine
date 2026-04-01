/**
 * @file AudioBackendFactory.h
 * @brief Factory for creating the best available IAudioBackend at startup
 * @author Spark Engine Team
 * @date 2026
 *
 * Auto-detects the platform and available audio libraries, then creates the
 * most capable backend. Falls back to NullAudioBackend if nothing is available.
 *
 * Priority order:
 *   1. XAudio2 (Windows)
 *   2. OpenAL Soft (Linux/macOS)
 *   3. NullAudioBackend (headless / no audio hardware)
 */

#pragma once

#include "IAudioBackend.h"

#include <memory>
#include <string>

class AudioEngine; // Forward declare for XAudio2 backend on Windows

namespace Spark::Audio
{

    /**
     * @brief Identifiers for supported audio backends
     */
    enum class AudioBackendType
    {
        Auto,    ///< Auto-detect best available
        XAudio2, ///< Windows XAudio2 (via AudioEngine)
        OpenAL,  ///< OpenAL Soft (Linux/macOS)
        Null     ///< Silent no-op backend
    };

    /**
     * @brief Create the best available audio backend
     *
     * When type is Auto, selects in priority order:
     *   Windows: XAudio2 > Null
     *   Linux/macOS: OpenAL > Null
     *
     * @param type Desired backend (Auto for platform default)
     * @param engine Existing AudioEngine instance (required for XAudio2 backend, nullptr otherwise)
     * @return Unique pointer to the created backend (never null)
     */
    std::unique_ptr<IAudioBackend> CreateAudioBackend(AudioBackendType type = AudioBackendType::Auto,
                                                      AudioEngine* engine = nullptr);

    /**
     * @brief Get a human-readable name for a backend type
     * @param type Backend type
     * @return String name (e.g., "XAudio2", "OpenAL", "Null")
     */
    const char* GetBackendName(AudioBackendType type);

} // namespace Spark::Audio
