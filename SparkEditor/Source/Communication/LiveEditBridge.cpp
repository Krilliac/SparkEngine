/**
 * @file LiveEditBridge.cpp
 * @brief Bridge between collaborative editing and a running AreaServer
 */

#include "LiveEditBridge.h"
#include "CollaborativeEditSession.h"
#include "Utils/Validate.h"

#include <cstring>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define CLOSE_SOCKET closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define CLOSE_SOCKET ::close
#endif

namespace SparkEditor
{

    namespace
    {
        // Reuse the wire protocol helpers from CollaborativeEditSession
        bool SendAll(int sock, const void* buf, size_t len)
        {
            const auto* ptr = static_cast<const char*>(buf);
            size_t sent = 0;
            while (sent < len)
            {
                auto n = ::send(sock, ptr + sent, static_cast<int>(len - sent), 0);
                if (n <= 0)
                    return false;
                sent += static_cast<size_t>(n);
            }
            return true;
        }

        bool SendFramedBridge(int sock, const std::vector<uint8_t>& data)
        {
            if (sock < 0)
                return false;

            uint32_t len = static_cast<uint32_t>(data.size());
            uint8_t header[4];
            header[0] = static_cast<uint8_t>((len >> 24) & 0xFF);
            header[1] = static_cast<uint8_t>((len >> 16) & 0xFF);
            header[2] = static_cast<uint8_t>((len >> 8) & 0xFF);
            header[3] = static_cast<uint8_t>(len & 0xFF);

            return SendAll(sock, header, 4) && SendAll(sock, data.data(), data.size());
        }

        void WriteU8(std::vector<uint8_t>& buf, uint8_t val)
        {
            buf.push_back(val);
        }

        void WriteU16(std::vector<uint8_t>& buf, uint16_t val)
        {
            buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(val & 0xFF));
        }

        void WriteU32(std::vector<uint8_t>& buf, uint32_t val)
        {
            buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
            buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>(val & 0xFF));
        }

        void WriteU64(std::vector<uint8_t>& buf, uint64_t val)
        {
            WriteU32(buf, static_cast<uint32_t>((val >> 32) & 0xFFFFFFFF));
            WriteU32(buf, static_cast<uint32_t>(val & 0xFFFFFFFF));
        }

        void WriteString(std::vector<uint8_t>& buf, const std::string& str)
        {
            WriteU32(buf, static_cast<uint32_t>(str.size()));
            buf.insert(buf.end(), str.begin(), str.end());
        }

        // Serialize an EditMessage into the AreaServer wire format
        std::vector<uint8_t> SerializeEditForServer(const EditMessage& edit)
        {
            std::vector<uint8_t> buf;
            buf.reserve(128);

            // Message type header: EditorNetMessageType::SceneEdit
            WriteU16(buf, static_cast<uint16_t>(EditorNetMessageType::SceneEdit));
            WriteU8(buf, static_cast<uint8_t>(edit.type));
            WriteU32(buf, edit.sourceEditor);
            WriteString(buf, edit.nodeId);
            WriteString(buf, edit.componentType);
            WriteString(buf, edit.propertyName);
            WriteString(buf, edit.newValue);
            WriteString(buf, edit.oldValue);
            WriteU64(buf, edit.timestamp);

            return buf;
        }

        std::vector<uint8_t> SerializeEditorJoin(const std::string& editorName)
        {
            std::vector<uint8_t> buf;
            buf.reserve(64);
            WriteU16(buf, static_cast<uint16_t>(EditorNetMessageType::EditorJoin));
            WriteString(buf, editorName);
            return buf;
        }
    } // namespace

    LiveEditBridge::LiveEditBridge() = default;

    LiveEditBridge::~LiveEditBridge()
    {
        Disconnect();
    }

    bool LiveEditBridge::Connect(const std::string& address, uint16_t port, const std::string& editorName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !address.empty(), false);

        if (m_connected.load(std::memory_order_acquire))
        {
            SPARK_LOG_WARN(Spark::LogCategory::Editor, "LiveEditBridge already connected.");
            return false;
        }

        m_socket = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
        if (m_socket < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LiveEditBridge: Failed to create socket.");
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, address.c_str(), &addr.sin_addr);

        if (::connect(m_socket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            SPARK_LOG_ERROR(Spark::LogCategory::Editor, "LiveEditBridge: Failed to connect to %s:%u.", address.c_str(),
                            port);
            CLOSE_SOCKET(m_socket);
            m_socket = -1;
            return false;
        }

        m_serverAddress = address;
        m_serverPort = port;
        m_editorName = editorName;
        m_shuttingDown.store(false, std::memory_order_release);
        m_connected.store(true, std::memory_order_release);

        // Send join handshake
        auto joinMsg = SerializeEditorJoin(editorName);
        SendFramedBridge(m_socket, joinMsg);

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "LiveEditBridge: Connected to AreaServer at %s:%u as '%s'.",
                       address.c_str(), port, editorName.c_str());
        return true;
    }

    void LiveEditBridge::Disconnect()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        if (!m_connected.load(std::memory_order_acquire))
            return;

        m_shuttingDown.store(true, std::memory_order_release);
        m_connected.store(false, std::memory_order_release);

        if (m_socket >= 0)
        {
            // Send leave notification before closing
            std::vector<uint8_t> leaveMsg;
            WriteU16(leaveMsg, static_cast<uint16_t>(EditorNetMessageType::EditorLeave));
            WriteString(leaveMsg, m_editorName);
            SendFramedBridge(m_socket, leaveMsg);

            CLOSE_SOCKET(m_socket);
            m_socket = -1;
        }

        if (m_sendThread.joinable())
            m_sendThread.join();

        SPARK_LOG_INFO(Spark::LogCategory::Editor, "LiveEditBridge: Disconnected from AreaServer.");
    }

    void LiveEditBridge::PushEdit(const EditMessage& edit)
    {
        if (!m_connected.load(std::memory_order_acquire))
            return;

        std::lock_guard<std::mutex> lock(m_editMutex);
        m_pendingEdits.push(edit);
    }

    void LiveEditBridge::Update()
    {
        if (!m_connected.load(std::memory_order_acquire))
            return;

        // Drain pending edits and send to AreaServer
        std::queue<EditMessage> editsToSend;
        {
            std::lock_guard<std::mutex> lock(m_editMutex);
            std::swap(editsToSend, m_pendingEdits);
        }

        while (!editsToSend.empty())
        {
            auto data = SerializeEditForServer(editsToSend.front());
            if (!SendFramedBridge(m_socket, data))
            {
                SPARK_LOG_WARN(Spark::LogCategory::Editor, "LiveEditBridge: Send failed, disconnecting.");
                m_connected.store(false, std::memory_order_release);
                break;
            }
            m_editsPushed++;
            editsToSend.pop();
        }
    }

} // namespace SparkEditor
