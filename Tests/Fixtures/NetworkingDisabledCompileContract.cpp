// The stable-v1 shipping preset deliberately omits ENABLE_NETWORKING.
// This translation unit must still be able to include public networking
// integration headers without inheriting networking-only implementation types.
#ifdef ENABLE_NETWORKING
#error "This contract must build only in an ENABLE_NETWORKING=OFF configuration."
#endif

#include "Engine/Networking/DedicatedServer.h"
#include "Engine/Networking/NetworkIntegration.h"

// SEC-100 / OD-05: remote administration is permanently unavailable in
// stable-v1. With networking compiled out, the dedicated server and its
// in-process RCON dispatch must not exist at all. These definitions are a
// redefinition error, failing this build, if DedicatedServer.h ever declares
// either type in this configuration.
namespace Spark::Net
{
    struct ServerConfig
    {
    };

    class DedicatedServer
    {
    };
} // namespace Spark::Net

int main()
{
    Spark::Net::ServerConfig localConfig;
    Spark::Net::DedicatedServer localServer;
    (void)localConfig;
    (void)localServer;
    return 0;
}
