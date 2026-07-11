/**
 * @file TFUiSounds.h
 * @brief Semantic UI bleep helper: terminal open/close/confirm/deny and
 *        ability activation/end cues, all mapped onto the shipped
 *        Audio/MMOFPS/ui/terminal_bleep_01..04.wav clips.
 *
 * OWNERSHIP: audio-wave-2 lane (W10), alongside Game/TFFootsteps.h/.cpp.
 *
 * Header-only (JsonUtils precedent) so the call sites in other lanes' files
 * stay ONE line: `TFUiSounds_Play(m_ctx, TFUiBleep::Open);`. Client-side
 * presentation only — never touches movement/collision/authority state, so
 * the determinism contract is untouched. Safe no-op when headless (GetAudio
 * null) or on a dedicated server (HasLocalPlayer false).
 *
 * No bespoke ability clips shipped in W10, so ability cues deliberately reuse
 * bleep_04 at low volume / shifted pitch instead of adding placeholder noise
 * (per the wave brief: tasteful reuse or skip).
 *
 * Function-local statics are fine here: this header is only included by
 * module (SparkGameMMOFPS DLL) translation units, so there is exactly one
 * image-local copy of the state (per-image DLL statics gotcha respected).
 */
#pragma once

#include "Core/TFTypes.h"

#include "Audio/AudioEngine.h"
#include "Spark/IEngineContext.h"
#include "Utils/SparkConsole.h"

#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_set>

namespace Terrafront
{

    /// Semantic cue kinds — call sites say WHAT happened, this header owns
    /// which clip/volume/pitch that maps to.
    enum class TFUiBleep : uint8_t
    {
        Open = 0,   ///< terminal/menu opened
        Close,      ///< terminal/menu closed (softer, lower)
        Confirm,    ///< action accepted (travel accepted, vehicle purchase sent)
        Deny,       ///< action refused (travel rejected)
        AbilityOn,  ///< local class ability became Active
        AbilityOff, ///< local class ability ended (cooldown/toggle-off)
        COUNT
    };

    /// Play one UI bleep. Lazy-loads the clip on first use (TFWeaponSystem::
    /// PlayWeaponAudio pattern); volumes stay subtle — these fire on every
    /// interaction, so they must never dominate the mix.
    inline void TFUiSounds_Play(const TFGameContext* ctx, TFUiBleep kind)
    {
        if (!ctx || !ctx->engine || !ctx->HasLocalPlayer())
            return;
        ::AudioEngine* audio = ctx->engine->GetAudio();
        if (!audio)
            return;

        struct Cue
        {
            const char* path;
            float vol;
            float pitch;
        };
        static constexpr Cue kCues[] = {
            {"Audio/MMOFPS/ui/terminal_bleep_01.wav", 0.45f, 1.00f}, // Open
            {"Audio/MMOFPS/ui/terminal_bleep_01.wav", 0.28f, 0.85f}, // Close
            {"Audio/MMOFPS/ui/terminal_bleep_02.wav", 0.50f, 1.00f}, // Confirm
            {"Audio/MMOFPS/ui/terminal_bleep_03.wav", 0.45f, 0.75f}, // Deny
            {"Audio/MMOFPS/ui/terminal_bleep_04.wav", 0.38f, 1.00f}, // AbilityOn
            {"Audio/MMOFPS/ui/terminal_bleep_04.wav", 0.26f, 0.80f}, // AbilityOff
        };
        static_assert(std::size(kCues) == static_cast<size_t>(TFUiBleep::COUNT), "one cue per TFUiBleep");
        const size_t idx = static_cast<size_t>(kind);
        if (idx >= std::size(kCues))
            return;
        const Cue& cue = kCues[idx];

        static std::unordered_set<std::string> s_loaded; // game-thread only; one copy in the module image
        if (s_loaded.insert(cue.path).second)
        {
            const std::string full = std::string("Assets/") + cue.path; // module paths are Assets-relative
            if (FAILED(audio->LoadSound(cue.path, std::wstring(full.begin(), full.end()))))
                Spark::SimpleConsole::GetInstance().LogWarning(std::string("[TFAudio] ui bleep load FAIL ") + cue.path);
        }
        audio->PlaySound(cue.path, cue.vol, cue.pitch);
    }

    /// Rising/falling edge helper for the local ability mirror: plays
    /// AbilityOn when `isActive` rises, AbilityOff when it falls — lets the
    /// TFAbilitySystem::ClientHandleState hook stay a single line.
    inline void TFUiSounds_ActiveEdge(const TFGameContext* ctx, bool wasActive, bool isActive)
    {
        if (isActive && !wasActive)
            TFUiSounds_Play(ctx, TFUiBleep::AbilityOn);
        else if (!isActive && wasActive)
            TFUiSounds_Play(ctx, TFUiBleep::AbilityOff);
    }

} // namespace Terrafront
