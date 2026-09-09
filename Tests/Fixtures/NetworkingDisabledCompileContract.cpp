// The stable-v1 shipping preset deliberately omits ENABLE_NETWORKING.
// This translation unit must still be able to include public networking
// integration headers without inheriting networking-only implementation types.
#ifdef ENABLE_NETWORKING
#error "This contract must build only in an ENABLE_NETWORKING=OFF configuration."
#endif

#include "Engine/Networking/NetworkIntegration.h"

int main()
{
    return 0;
}
