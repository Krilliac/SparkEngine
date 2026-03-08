/**
 * @file AudioSystemEnums.h
 * @brief Enumerations for the audio system
 * @author Spark Engine Team
 * @date 2025
 * 
 * This file contains all enumerations related to audio processing,
 * including audio formats, playback states, and mixing modes.
 */

#pragma once

namespace SparkEditor
{

    /**
 * @brief Audio file formats
 */
    enum class AudioFormat
    {
        WAV = 0,  ///< WAV format
        MP3 = 1,  ///< MP3 format
        OGG = 2,  ///< OGG Vorbis format
        FLAC = 3, ///< FLAC format
        AAC = 4,  ///< AAC format
        AIFF = 5  ///< AIFF format
    };

    /**
 * @brief Audio playback states
 */
    enum class AudioPlaybackState
    {
        STOPPED = 0, ///< Audio is stopped
        PLAYING = 1, ///< Audio is playing
        PAUSED = 2,  ///< Audio is paused
        LOADING = 3, ///< Audio is loading
        ERROR = 4    ///< Error in audio playback
    };

    /**
 * @brief Audio channel configurations
 */
    enum class ChannelConfiguration
    {
        MONO = 1,         ///< Mono (1 channel)
        STEREO = 2,       ///< Stereo (2 channels)
        SURROUND_5_1 = 6, ///< 5.1 Surround (6 channels)
        SURROUND_7_1 = 8  ///< 7.1 Surround (8 channels)
    };

    /**
 * @brief Audio quality settings
 */
    enum class AudioQuality
    {
        LOW = 0,    ///< Low quality (22kHz, 8-bit)
        MEDIUM = 1, ///< Medium quality (44kHz, 16-bit)
        HIGH = 2,   ///< High quality (48kHz, 24-bit)
        ULTRA = 3   ///< Ultra quality (96kHz, 32-bit)
    };

    /**
 * @brief Audio effects types
 */
    enum class AudioEffect
    {
        REVERB = 0,     ///< Reverb effect
        ECHO = 1,       ///< Echo effect
        CHORUS = 2,     ///< Chorus effect
        DISTORTION = 3, ///< Distortion effect
        COMPRESSOR = 4, ///< Audio compressor
        EQUALIZER = 5,  ///< Graphic equalizer
        LOW_PASS = 6,   ///< Low-pass filter
        HIGH_PASS = 7,  ///< High-pass filter
        BAND_PASS = 8   ///< Band-pass filter
    };

    /**
 * @brief Audio source types
 */
    enum class AudioSourceType
    {
        BACKGROUND_MUSIC = 0, ///< Background music
        SOUND_EFFECT = 1,     ///< Sound effects
        VOICE = 2,            ///< Voice/dialogue
        AMBIENT = 3,          ///< Ambient sounds
        UI = 4                ///< UI sounds
    };

    /**
 * @brief 3D audio modes
 */
    enum class Audio3DMode
    {
        STEREO_2D = 0,     ///< 2D stereo audio
        POSITIONAL_3D = 1, ///< 3D positional audio
        BINAURAL = 2,      ///< Binaural 3D audio
        HRTF = 3           ///< HRTF-based 3D audio
    };

    /**
 * @brief Audio compression types
 */
    enum class AudioCompressionType
    {
        NONE = 0,         ///< No compression
        LOSSLESS = 1,     ///< Lossless compression
        LOSSY_LOW = 2,    ///< Low quality lossy compression
        LOSSY_MEDIUM = 3, ///< Medium quality lossy compression
        LOSSY_HIGH = 4    ///< High quality lossy compression
    };

} // namespace SparkEditor