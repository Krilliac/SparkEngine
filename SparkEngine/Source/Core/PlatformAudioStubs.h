/**
 * @file PlatformAudioStubs.h
 * @brief XAudio2, XInput, and related stubs for non-Windows platforms
 *
 * Provides no-op stub implementations of XAudio2 interfaces, XInput structs,
 * and related constants so that audio and input code compiles on Linux and macOS.
 */

#pragma once

#ifndef SPARK_PLATFORM_WINDOWS

#include "PlatformTypes.h"

#include <cstdio>

// --- XAudio2 stubs ---
#ifndef XAUDIO2_DEFAULT_PROCESSOR
#define XAUDIO2_DEFAULT_PROCESSOR 1
#endif
#ifndef XAUDIO2_END_OF_STREAM
#define XAUDIO2_END_OF_STREAM 0x0040
#endif
#ifndef XAUDIO2_LOOP_INFINITE
#define XAUDIO2_LOOP_INFINITE 255
#endif

struct XAUDIO2_BUFFER
{
    uint32_t Flags;
    uint32_t AudioBytes;
    const uint8_t* pAudioData;
    uint32_t PlayBegin;
    uint32_t PlayLength;
    uint32_t LoopBegin;
    uint32_t LoopLength;
    uint32_t LoopCount;
    void* pContext;
};
struct XAUDIO2_VOICE_STATE
{
    void* pCurrentBufferContext;
    uint32_t BuffersQueued;
    uint64_t SamplesPlayed;
};
struct XAUDIO2_VOICE_DETAILS
{
    uint32_t CreationFlags;
    uint32_t ActiveFlags;
    uint32_t InputChannels;
    uint32_t InputSampleRate;
};

// Stub XAudio2 interfaces with no-op methods so code compiles on Linux
struct IXAudio2SourceVoice
{
    virtual ~IXAudio2SourceVoice() = default;
    long SetVolume(float) { return 0; }
    long SetFrequencyRatio(float) { return 0; }
    long SubmitSourceBuffer(const XAUDIO2_BUFFER*) { return 0; }
    long Start(uint32_t = 0, uint32_t = 0) { return 0; }
    long Stop(uint32_t = 0, uint32_t = 0) { return 0; }
    long FlushSourceBuffers() { return 0; }
    void GetState(XAUDIO2_VOICE_STATE* s)
    {
        if (s)
        {
            s->BuffersQueued = 0;
            s->SamplesPlayed = 0;
            s->pCurrentBufferContext = nullptr;
        }
    }
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* d)
    {
        if (d)
        {
            d->InputChannels = 1;
            d->InputSampleRate = 44100;
            d->CreationFlags = 0;
            d->ActiveFlags = 0;
        }
    }
    long SetOutputMatrix(void*, uint32_t, uint32_t, const float*) { return 0; }
    void DestroyVoice() {}
    void Release() {}
};

struct IXAudio2MasteringVoice
{
    virtual ~IXAudio2MasteringVoice() = default;
    long SetVolume(float) { return 0; }
    void GetVoiceDetails(XAUDIO2_VOICE_DETAILS* d)
    {
        if (d)
        {
            d->InputChannels = 2;
            d->InputSampleRate = 44100;
            d->CreationFlags = 0;
            d->ActiveFlags = 0;
        }
    }
    void DestroyVoice() {}
    void Release() {}
};

struct IXAudio2SubmixVoice
{
    virtual ~IXAudio2SubmixVoice() = default;
    void DestroyVoice() {}
};

struct IXAudio2
{
    virtual ~IXAudio2() = default;
    long CreateMasteringVoice(IXAudio2MasteringVoice** pp, uint32_t = 0, uint32_t = 0, uint32_t = 0, void* = nullptr,
                              void* = nullptr)
    {
        static IXAudio2MasteringVoice stubMaster;
        if (pp)
            *pp = &stubMaster;
        return 0;
    }
    long CreateSourceVoice(IXAudio2SourceVoice** pp, const WAVEFORMATEX* = nullptr, uint32_t = 0, float = 2.0f,
                           void* = nullptr, void* = nullptr, void* = nullptr)
    {
        static IXAudio2SourceVoice stubSource;
        if (pp)
            *pp = &stubSource;
        return 0;
    }
    long CreateSubmixVoice(IXAudio2SubmixVoice** pp, uint32_t = 2, uint32_t = 44100, uint32_t = 0, uint32_t = 0,
                           void* = nullptr, void* = nullptr)
    {
        static IXAudio2SubmixVoice stubSubmix;
        if (pp)
            *pp = &stubSubmix;
        return 0;
    }
    void Release() {}
};

// Wine wraps XAudio2 over ALSA/PulseAudio on Linux. SparkEngine uses no-op
// stubs for now — a native SDL2 audio backend is planned but not yet implemented.
inline long XAudio2Create(IXAudio2** pp, uint32_t = 0, uint32_t = XAUDIO2_DEFAULT_PROCESSOR)
{
    static bool warned = false;
    if (!warned)
    {
        fprintf(stderr, "[SparkEngine] Warning: XAudio2 unavailable on this platform. Audio is disabled.\n");
        warned = true;
    }
    static IXAudio2 stubEngine;
    if (pp)
        *pp = &stubEngine;
    return 0; // S_OK — succeeds silently so callers don't crash
}

// --- XInput stubs ---
struct XINPUT_GAMEPAD
{
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    short sThumbLX;
    short sThumbLY;
    short sThumbRX;
    short sThumbRY;
};
struct XINPUT_STATE
{
    uint32_t dwPacketNumber;
    XINPUT_GAMEPAD Gamepad;
};
struct XINPUT_VIBRATION
{
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};

#ifndef XINPUT_GAMEPAD_A
#define XINPUT_GAMEPAD_DPAD_UP 0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN 0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT 0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT 0x0008
#define XINPUT_GAMEPAD_START 0x0010
#define XINPUT_GAMEPAD_BACK 0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB 0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB 0x0080
#define XINPUT_GAMEPAD_LEFT_SHOULDER 0x0100
#define XINPUT_GAMEPAD_RIGHT_SHOULDER 0x0200
#define XINPUT_GAMEPAD_A 0x1000
#define XINPUT_GAMEPAD_B 0x2000
#define XINPUT_GAMEPAD_X 0x4000
#define XINPUT_GAMEPAD_Y 0x8000
#endif

// Wine maps XInput to Linux's evdev/joydev. SparkEngine uses SDL2's gamepad API
// which provides near-identical abstraction (button/axis mapping, rumble).
#ifdef SPARK_SDL2_AVAILABLE
#include <SDL2/SDL.h>

inline uint32_t XInputGetState(uint32_t dwUserIndex, XINPUT_STATE* pState)
{
    if (!pState || dwUserIndex > 3)
        return 1; // ERROR_DEVICE_NOT_CONNECTED

    SDL_GameController* controller = SDL_GameControllerOpen(static_cast<int>(dwUserIndex));
    if (!controller)
        return 1;

    memset(pState, 0, sizeof(XINPUT_STATE));

    // Map SDL2 buttons → XInput button flags
    auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(controller, b); };
    uint16_t buttons = 0;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP))
        buttons |= XINPUT_GAMEPAD_DPAD_UP;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT))
        buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (btn(SDL_CONTROLLER_BUTTON_START))
        buttons |= XINPUT_GAMEPAD_START;
    if (btn(SDL_CONTROLLER_BUTTON_BACK))
        buttons |= XINPUT_GAMEPAD_BACK;
    if (btn(SDL_CONTROLLER_BUTTON_LEFTSTICK))
        buttons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK))
        buttons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))
        buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
        buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (btn(SDL_CONTROLLER_BUTTON_A))
        buttons |= XINPUT_GAMEPAD_A;
    if (btn(SDL_CONTROLLER_BUTTON_B))
        buttons |= XINPUT_GAMEPAD_B;
    if (btn(SDL_CONTROLLER_BUTTON_X))
        buttons |= XINPUT_GAMEPAD_X;
    if (btn(SDL_CONTROLLER_BUTTON_Y))
        buttons |= XINPUT_GAMEPAD_Y;

    pState->Gamepad.wButtons = buttons;

    // Map SDL2 axes → XInput axes (SDL range: -32768..32767, matches XInput)
    auto axis = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(controller, a); };
    pState->Gamepad.sThumbLX = static_cast<short>(axis(SDL_CONTROLLER_AXIS_LEFTX));
    pState->Gamepad.sThumbLY = static_cast<short>(-axis(SDL_CONTROLLER_AXIS_LEFTY)); // Y-axis inverted
    pState->Gamepad.sThumbRX = static_cast<short>(axis(SDL_CONTROLLER_AXIS_RIGHTX));
    pState->Gamepad.sThumbRY = static_cast<short>(-axis(SDL_CONTROLLER_AXIS_RIGHTY));
    pState->Gamepad.bLeftTrigger = static_cast<uint8_t>(axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7);
    pState->Gamepad.bRightTrigger = static_cast<uint8_t>(axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7);

    return 0; // ERROR_SUCCESS
}

inline uint32_t XInputSetState(uint32_t dwUserIndex, XINPUT_VIBRATION* pVibration)
{
    if (!pVibration || dwUserIndex > 3)
        return 1;

    SDL_GameController* controller = SDL_GameControllerOpen(static_cast<int>(dwUserIndex));
    if (!controller)
        return 1;

    SDL_GameControllerRumble(controller, pVibration->wLeftMotorSpeed, pVibration->wRightMotorSpeed, 100);
    return 0;
}

#else // No SDL2 — dead stubs

inline uint32_t XInputGetState(uint32_t, XINPUT_STATE*)
{
    return 1; // ERROR_DEVICE_NOT_CONNECTED
}
inline uint32_t XInputSetState(uint32_t, XINPUT_VIBRATION*)
{
    return 1;
}

#endif // SPARK_SDL2_AVAILABLE

// --- Win32 process/module stubs ---
#ifndef PROCESS_QUERY_INFORMATION
#define PROCESS_QUERY_INFORMATION 0x0400
#endif
#ifndef GetCurrentThreadId
#define GetCurrentThreadId SparkGetCurrentThreadId
#endif

// --- Console color constants ---
#ifndef FOREGROUND_RED
#define FOREGROUND_RED 0x0004
#define FOREGROUND_GREEN 0x0002
#define FOREGROUND_BLUE 0x0001
#define FOREGROUND_INTENSITY 0x0008
#endif

#endif // !SPARK_PLATFORM_WINDOWS
