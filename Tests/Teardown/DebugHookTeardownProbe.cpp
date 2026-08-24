/**
 * @file DebugHookTeardownProbe.cpp
 * @brief Process-boundary regression probe for debug-hook static lifetime.
 */

#include "Engine/Networking/NetworkManager.h"

int main()
{
    // Construct NetworkManager first, then DebugHookManager from Initialize().
    // Returning without explicit Shutdown forces NetworkManager's fallback
    // singleton to dispatch its shutdown hooks during CRT static teardown.
    // The process must exit cleanly regardless of destruction order.
    return Spark::Net::NetworkManager::GetInstance().Initialize() ? 0 : 1;
}
