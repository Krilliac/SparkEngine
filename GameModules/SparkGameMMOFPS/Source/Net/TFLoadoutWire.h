/**
 * @file TFLoadoutWire.h
 * @brief TF-110: decode a client TF_LoadoutChange into weapon keys without
 *        letting a forged WeaponId fall through to "class default".
 *
 * kInvalidWeapon is the wire's explicit "use the class default" marker and
 * maps to an empty key (TFProgressionSystem::ValidLoadoutSlotKey treats an
 * empty key as the class default). Any OTHER id must resolve to a real weapon;
 * an id that does not resolve is forged state and the whole message is
 * rejected (std::nullopt) so the caller can audit it via
 * TFServerValidation::RecordForgedStateReject. Header-only so the decode rule
 * is unit-testable without the module's data tables.
 */
#pragma once

#include "Core/TFTypes.h"
#include "Net/TFNetProtocol.h"

#include <cstdint>
#include <optional>
#include <string>

namespace Terrafront
{

    struct TFLoadoutWireKeys
    {
        std::string primary;
        std::string secondary;
        std::string tool;
    };

    /// `resolve(uint16_t id) -> const std::string*` returns the weapon key for
    /// a known id and nullptr for an unknown one.
    template <class Resolve>
    std::optional<TFLoadoutWireKeys> DecodeLoadoutChange(const TF_LoadoutChange& msg, Resolve resolve)
    {
        TFLoadoutWireKeys keys;
        auto decodeSlot = [&resolve](uint16_t wid, std::string& out) -> bool
        {
            if (wid == kInvalidWeapon)
                return true; // explicit class default
            const std::string* key = resolve(wid);
            if (!key || key->empty())
                return false; // forged / unknown id: never silently defaulted
            out = *key;
            return true;
        };
        if (!decodeSlot(msg.primary, keys.primary) || !decodeSlot(msg.secondary, keys.secondary) ||
            !decodeSlot(msg.tool, keys.tool))
            return std::nullopt;
        return keys;
    }

} // namespace Terrafront
