/**
 * @file DaemonConnection.cpp
 * @brief Implementation of the process-wide DaemonConnection singleton.
 */

#include "DaemonConnection.h"
#include "DaemonProtocol.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace Spark::Daemon
{

    DaemonConnection& DaemonConnection::Instance()
    {
        static DaemonConnection s_instance;
        return s_instance;
    }

    bool DaemonConnection::TryConnect(const std::string& socketPath)
    {
        std::lock_guard lock(m_mutex);

        if (m_client && m_client->IsConnected())
            return true; // Already connected.

        std::string path = socketPath.empty() ? std::string("./") + kDefaultSocketName : socketPath;

#if !defined(_WIN32)
        struct stat st;
        if (::stat(path.c_str(), &st) != 0)
            return false; // No daemon socket — nothing to connect to.
#endif

        auto client = std::make_unique<DaemonClient>();
        auto result = client->Connect(path);
        if (!result)
            return false;

        m_client = std::move(client);
        m_socketPath = path;
        return true;
    }

    void DaemonConnection::Shutdown()
    {
        std::lock_guard lock(m_mutex);
        if (m_client)
            m_client->Disconnect();
        m_client.reset();
        m_socketPath.clear();
    }

    bool DaemonConnection::IsConnected() const
    {
        std::lock_guard lock(m_mutex);
        return m_client && m_client->IsConnected();
    }

    DaemonClient* DaemonConnection::GetClient()
    {
        std::lock_guard lock(m_mutex);
        if (!m_client || !m_client->IsConnected())
            return nullptr;
        return m_client.get();
    }

    std::string DaemonConnection::GetSocketPath() const
    {
        std::lock_guard lock(m_mutex);
        return m_socketPath;
    }

} // namespace Spark::Daemon
