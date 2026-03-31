/**
 * miniaudio - v0.11.x - public domain audio playback and capture library
 * https://github.com/mackron/miniaudio
 *
 * This is a MINIMAL STUB providing the miniaudio API surface used by
 * SparkEngine's cross-platform audio backend. For production use, replace
 * with the real miniaudio.h from:
 *   curl -L https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h -o ThirdParty/Audio/miniaudio/miniaudio.h
 *
 * License: Public Domain / MIT-0
 */

#ifndef MINIAUDIO_H
#define MINIAUDIO_H

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C"
{
#endif

    // =========================================================================
    // Core types
    // =========================================================================

    typedef int32_t ma_result;
#define MA_SUCCESS 0
#define MA_ERROR -1
#define MA_INVALID_ARGS -2
#define MA_INVALID_OPERATION -3
#define MA_OUT_OF_MEMORY -4
#define MA_FORMAT_NOT_SUPPORTED -5
#define MA_DEVICE_NOT_INITIALIZED -6

    typedef uint8_t ma_bool8;
    typedef uint32_t ma_bool32;
#define MA_TRUE 1
#define MA_FALSE 0

    typedef enum
    {
        ma_format_unknown = 0,
        ma_format_u8 = 1,
        ma_format_s16 = 2,
        ma_format_s24 = 3,
        ma_format_s32 = 4,
        ma_format_f32 = 5,
        ma_format_count
    } ma_format;

    typedef enum
    {
        ma_device_type_playback = 1,
        ma_device_type_capture = 2,
        ma_device_type_duplex = 3,
        ma_device_type_loopback = 4
    } ma_device_type;

    typedef enum
    {
        ma_backend_wasapi,
        ma_backend_dsound,
        ma_backend_winmm,
        ma_backend_coreaudio,
        ma_backend_sndio,
        ma_backend_audio4,
        ma_backend_oss,
        ma_backend_pulseaudio,
        ma_backend_alsa,
        ma_backend_jack,
        ma_backend_aaudio,
        ma_backend_opensl,
        ma_backend_webaudio,
        ma_backend_custom,
        ma_backend_null
    } ma_backend;

    typedef enum
    {
        ma_channel_mix_mode_rectangular = 0,
        ma_channel_mix_mode_simple,
        ma_channel_mix_mode_custom_weights,
        ma_channel_mix_mode_default = ma_channel_mix_mode_rectangular
    } ma_channel_mix_mode;

    // =========================================================================
    // Engine / Device
    // =========================================================================

    typedef struct ma_device_config
    {
        ma_device_type deviceType;
        uint32_t sampleRate;
        uint32_t channels;
        ma_format format;
        uint32_t periodSizeInFrames;
        uint32_t periods;
        void (*dataCallback)(struct ma_device* pDevice, void* pOutput, const void* pInput, uint32_t frameCount);
        void* pUserData;
    } ma_device_config;

    typedef struct ma_device
    {
        ma_device_config config;
        ma_bool32 isStarted;
        void* pUserData;
    } ma_device;

    typedef struct ma_engine_config
    {
        uint32_t channels;
        uint32_t sampleRate;
        uint32_t listenerCount;
    } ma_engine_config;

    typedef struct ma_engine
    {
        ma_device device;
        ma_engine_config config;
        ma_bool32 isInitialized;
    } ma_engine;

    typedef struct ma_sound
    {
        ma_engine* pEngine;
        ma_bool32 isPlaying;
        ma_bool32 isLooping;
        float volume;
        float pitch;
        float posX, posY, posZ;
        ma_bool32 spatialized;
    } ma_sound;

    // =========================================================================
    // Config helpers
    // =========================================================================

    ma_device_config ma_device_config_init(ma_device_type deviceType);
    ma_engine_config ma_engine_config_init(void);

    // =========================================================================
    // Engine API
    // =========================================================================

    ma_result ma_engine_init(const ma_engine_config* pConfig, ma_engine* pEngine);
    void ma_engine_uninit(ma_engine* pEngine);

    ma_result ma_engine_start(ma_engine* pEngine);
    ma_result ma_engine_stop(ma_engine* pEngine);

    ma_result ma_engine_set_volume(ma_engine* pEngine, float volume);
    float ma_engine_get_volume(ma_engine* pEngine);

    void ma_engine_listener_set_position(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z);
    void ma_engine_listener_set_direction(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z);
    void ma_engine_listener_set_world_up(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z);
    void ma_engine_listener_set_velocity(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z);

    // =========================================================================
    // Sound API
    // =========================================================================

    ma_result ma_sound_init_from_file(ma_engine* pEngine, const char* pFilePath, uint32_t flags, void* pGroup,
                                      void* pDoneFence, ma_sound* pSound);
    void ma_sound_uninit(ma_sound* pSound);

    ma_result ma_sound_start(ma_sound* pSound);
    ma_result ma_sound_stop(ma_sound* pSound);

    void ma_sound_set_volume(ma_sound* pSound, float volume);
    void ma_sound_set_pitch(ma_sound* pSound, float pitch);
    void ma_sound_set_looping(ma_sound* pSound, ma_bool32 isLooping);
    void ma_sound_set_position(ma_sound* pSound, float x, float y, float z);
    void ma_sound_set_spatialization_enabled(ma_sound* pSound, ma_bool32 enabled);

    ma_bool32 ma_sound_is_playing(const ma_sound* pSound);
    ma_bool32 ma_sound_at_end(const ma_sound* pSound);

#define MA_SOUND_FLAG_DECODE 0x00000001
#define MA_SOUND_FLAG_STREAM 0x00000002
#define MA_SOUND_FLAG_ASYNC 0x00000004
#define MA_SOUND_FLAG_NO_DEFAULT_ATTACHMENT 0x00000008
#define MA_SOUND_FLAG_NO_PITCH 0x00000010
#define MA_SOUND_FLAG_NO_SPATIALIZATION 0x00000020

#ifdef __cplusplus
}
#endif

// ============================================================================
// Implementation
// ============================================================================
#ifdef MINIAUDIO_IMPLEMENTATION

ma_device_config ma_device_config_init(ma_device_type deviceType)
{
    ma_device_config config;
    memset(&config, 0, sizeof(config));
    config.deviceType = deviceType;
    config.sampleRate = 44100;
    config.channels = 2;
    config.format = ma_format_f32;
    config.periodSizeInFrames = 256;
    config.periods = 3;
    return config;
}

ma_engine_config ma_engine_config_init(void)
{
    ma_engine_config config;
    memset(&config, 0, sizeof(config));
    config.channels = 2;
    config.sampleRate = 44100;
    config.listenerCount = 1;
    return config;
}

ma_result ma_engine_init(const ma_engine_config* pConfig, ma_engine* pEngine)
{
    if (!pEngine)
        return MA_INVALID_ARGS;

    memset(pEngine, 0, sizeof(*pEngine));
    if (pConfig)
        pEngine->config = *pConfig;
    else
        pEngine->config = ma_engine_config_init();

    pEngine->isInitialized = MA_TRUE;
    return MA_SUCCESS;
}

void ma_engine_uninit(ma_engine* pEngine)
{
    if (pEngine)
    {
        pEngine->isInitialized = MA_FALSE;
    }
}

ma_result ma_engine_start(ma_engine* pEngine)
{
    if (!pEngine || !pEngine->isInitialized)
        return MA_DEVICE_NOT_INITIALIZED;
    return MA_SUCCESS;
}

ma_result ma_engine_stop(ma_engine* pEngine)
{
    if (!pEngine)
        return MA_INVALID_ARGS;
    return MA_SUCCESS;
}

ma_result ma_engine_set_volume(ma_engine* pEngine, float volume)
{
    (void)pEngine;
    (void)volume;
    return MA_SUCCESS;
}

float ma_engine_get_volume(ma_engine* pEngine)
{
    (void)pEngine;
    return 1.0f;
}

void ma_engine_listener_set_position(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z)
{
    (void)pEngine;
    (void)listenerIndex;
    (void)x;
    (void)y;
    (void)z;
}

void ma_engine_listener_set_direction(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z)
{
    (void)pEngine;
    (void)listenerIndex;
    (void)x;
    (void)y;
    (void)z;
}

void ma_engine_listener_set_world_up(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z)
{
    (void)pEngine;
    (void)listenerIndex;
    (void)x;
    (void)y;
    (void)z;
}

void ma_engine_listener_set_velocity(ma_engine* pEngine, uint32_t listenerIndex, float x, float y, float z)
{
    (void)pEngine;
    (void)listenerIndex;
    (void)x;
    (void)y;
    (void)z;
}

ma_result ma_sound_init_from_file(ma_engine* pEngine, const char* pFilePath, uint32_t flags, void* pGroup,
                                   void* pDoneFence, ma_sound* pSound)
{
    (void)flags;
    (void)pGroup;
    (void)pDoneFence;
    (void)pFilePath;

    if (!pEngine || !pSound)
        return MA_INVALID_ARGS;

    memset(pSound, 0, sizeof(*pSound));
    pSound->pEngine = pEngine;
    pSound->volume = 1.0f;
    pSound->pitch = 1.0f;
    return MA_SUCCESS;
}

void ma_sound_uninit(ma_sound* pSound)
{
    if (pSound)
    {
        pSound->isPlaying = MA_FALSE;
    }
}

ma_result ma_sound_start(ma_sound* pSound)
{
    if (!pSound)
        return MA_INVALID_ARGS;
    pSound->isPlaying = MA_TRUE;
    return MA_SUCCESS;
}

ma_result ma_sound_stop(ma_sound* pSound)
{
    if (!pSound)
        return MA_INVALID_ARGS;
    pSound->isPlaying = MA_FALSE;
    return MA_SUCCESS;
}

void ma_sound_set_volume(ma_sound* pSound, float volume)
{
    if (pSound)
        pSound->volume = volume;
}

void ma_sound_set_pitch(ma_sound* pSound, float pitch)
{
    if (pSound)
        pSound->pitch = pitch;
}

void ma_sound_set_looping(ma_sound* pSound, ma_bool32 isLooping)
{
    if (pSound)
        pSound->isLooping = isLooping;
}

void ma_sound_set_position(ma_sound* pSound, float x, float y, float z)
{
    if (pSound)
    {
        pSound->posX = x;
        pSound->posY = y;
        pSound->posZ = z;
    }
}

void ma_sound_set_spatialization_enabled(ma_sound* pSound, ma_bool32 enabled)
{
    if (pSound)
        pSound->spatialized = enabled;
}

ma_bool32 ma_sound_is_playing(const ma_sound* pSound)
{
    return pSound ? pSound->isPlaying : MA_FALSE;
}

ma_bool32 ma_sound_at_end(const ma_sound* pSound)
{
    (void)pSound;
    return MA_FALSE;
}

#endif // MINIAUDIO_IMPLEMENTATION

#endif // MINIAUDIO_H
