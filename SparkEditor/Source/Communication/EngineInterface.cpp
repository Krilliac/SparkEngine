/**
 * @file EngineInterface.cpp
 * @brief Implementation of the Engine communication interface
 * @author Spark Engine Team
 * @date 2025
 */

#include "EngineInterface.h"
#include "Utils/Validate.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <random>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace SparkEditor
{

    EngineInterface::EngineInterface()
    {
        std::cout << "EngineInterface constructor called\n";
    }

    EngineInterface::~EngineInterface()
    {
        std::cout << "EngineInterface destructor called\n";
        Shutdown();
    }

    bool EngineInterface::Initialize(const std::string& pipeName)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_VALIDATE_RET(Spark::LogCategory::Editor, !pipeName.empty(), false);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EngineInterface initializing with pipe: %s", pipeName.c_str());
        std::cout << "EngineInterface::Initialize() with pipe: " << pipeName << "\n";

        m_pipeName = pipeName;
        m_isShuttingDown = false;

        // Initialize system info with default values
        m_systemInfo.version = "1.0.0";
        m_systemInfo.platform = "Windows";
        m_systemInfo.graphicsAPI = "DirectX 11";
        m_systemInfo.audioAPI = "XAudio2";
        m_systemInfo.debugMode = true;
        m_systemInfo.startTime =
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        // Initialize metrics with default values
        m_currentMetrics.fps = 60.0f;
        m_currentMetrics.frameTime = 16.67f;
        m_currentMetrics.cpuTime = 8.0f;
        m_currentMetrics.gpuTime = 8.0f;
        m_currentMetrics.memoryUsage = 512 * 1024 * 1024;    // 512MB
        m_currentMetrics.gpuMemoryUsage = 256 * 1024 * 1024; // 256MB
        m_currentMetrics.drawCalls = 150;
        m_currentMetrics.triangles = 50000;
        m_currentMetrics.activeObjects = 25;
        m_currentMetrics.isPlayMode = false;
        m_currentMetrics.timeScale = 1.0f;

        std::cout << "EngineInterface initialized successfully\n";
        return true;
    }

    void EngineInterface::Shutdown()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EngineInterface shutting down");
        std::cout << "EngineInterface::Shutdown()\n";

        m_isShuttingDown = true;

        if (m_isConnected)
        {
            Disconnect();
        }

        if (m_commThread && m_commThread->joinable())
        {
            m_commThread->join();
        }

        m_commThread.reset();
    }

    void EngineInterface::Update(float deltaTime)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        // Update metrics (simulated when not connected to a real engine)
        static float timeSinceUpdate = 0.0f;
        timeSinceUpdate += deltaTime;

        if (timeSinceUpdate >= 1.0f)
        {
            std::lock_guard<std::mutex> lock(m_metricsMutex);

            // Use a proper random engine instead of rand()
            static std::mt19937 rng(std::random_device{}());
            std::uniform_real_distribution<float> fpsDist(58.0f, 67.0f);
            std::uniform_real_distribution<float> cpuDist(6.0f, 11.0f);
            std::uniform_real_distribution<float> gpuDist(5.0f, 12.0f);
            std::uniform_int_distribution<int64_t> memDist(-1024, 1024);

            m_currentMetrics.fps = fpsDist(rng);
            m_currentMetrics.frameTime = 1000.0f / m_currentMetrics.fps;
            m_currentMetrics.cpuTime = cpuDist(rng);
            m_currentMetrics.gpuTime = gpuDist(rng);

            // Simulate memory usage fluctuation (+/- 1MB)
            m_currentMetrics.memoryUsage += memDist(rng) * 1024;
            m_currentMetrics.memoryUsage =
                std::clamp(m_currentMetrics.memoryUsage, static_cast<size_t>(256) * 1024 * 1024,
                           static_cast<size_t>(1024) * 1024 * 1024);

            timeSinceUpdate = 0.0f;
        }

        // Process incoming events if any
        ProcessIncomingEvents();

        // Process outgoing commands if any
        ProcessOutgoingCommands();

        // Handle connection events
        HandleConnectionEvents();
    }

    bool EngineInterface::Connect(float timeoutSeconds)
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EngineInterface connecting (timeout: %.1fs)", timeoutSeconds);
        std::cout << "Attempting to connect to engine (timeout: " << timeoutSeconds << "s)\n";

        // For now, simulate a connection
        // In a real implementation, this would attempt to connect to a named pipe

        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Simulate connection time

        m_isConnected = true;
        m_connectionStartTime = std::chrono::steady_clock::now();
        m_connectionStats.connectionAttempts++;

        std::cout << "Successfully connected to engine\n";

        // Start communication thread
        if (!m_commThread)
        {
            m_commThread = std::make_unique<std::thread>(&EngineInterface::CommunicationThread, this);
        }

        return true;
    }

    void EngineInterface::Disconnect()
    {
        SPARK_TRACE_ENTER(Spark::LogCategory::Editor);
        SPARK_LOG_INFO(Spark::LogCategory::Editor, "EngineInterface disconnecting");
        std::cout << "Disconnecting from engine\n";

        m_isConnected = false;
        m_connectionStats.disconnections++;

        if (m_pipeHandle)
        {
            // Close pipe handle (platform-specific cleanup would go here)
            m_pipeHandle = nullptr;
        }

        std::cout << "Disconnected from engine\n";
    }

    uint64_t EngineInterface::SendCommand(const EngineCommand& command)
    {
        if (!m_isConnected)
        {
            std::cout << "Cannot send command - not connected to engine\n";
            return 0;
        }

        std::lock_guard<std::mutex> lock(m_commandMutex);

        EngineCommand cmdCopy = command;
        cmdCopy.commandID = GenerateCommandID();
        cmdCopy.timestamp =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();

        m_outgoingCommands.push(cmdCopy);
        m_connectionStats.commandsSent++;

        std::cout << "Queued command type " << static_cast<uint32_t>(command.type) << " with ID " << cmdCopy.commandID
                  << "\n";

        return cmdCopy.commandID;
    }

    uint64_t EngineInterface::SendCommand(EngineCommandType type,
                                          const std::unordered_map<std::string, std::string>& parameters)
    {
        EngineCommand command;
        command.type = type;
        command.parameters = parameters;

        return SendCommand(command);
    }

    void EngineInterface::RegisterEventCallback(EngineEventType eventType,
                                                std::function<void(const EngineEvent&)> callback)
    {
        m_eventCallbacks[eventType] = callback;
        std::cout << "Registered callback for event type " << static_cast<uint32_t>(eventType) << "\n";
    }

    void EngineInterface::UnregisterEventCallback(EngineEventType eventType)
    {
        auto it = m_eventCallbacks.find(eventType);
        if (it != m_eventCallbacks.end())
        {
            m_eventCallbacks.erase(it);
            std::cout << "Unregistered callback for event type " << static_cast<uint32_t>(eventType) << "\n";
        }
    }

    EngineMetrics EngineInterface::GetEngineMetrics() const
    {
        std::lock_guard<std::mutex> lock(m_metricsMutex);
        return m_currentMetrics;
    }

    EngineSystemInfo EngineInterface::GetEngineSystemInfo() const
    {
        return m_systemInfo;
    }

    EngineInterface::ConnectionStats EngineInterface::GetConnectionStats() const
    {
        return m_connectionStats;
    }

    void EngineInterface::ClearConnectionStats()
    {
        m_connectionStats = ConnectionStats{};
        std::cout << "Connection statistics cleared\n";
    }

    void EngineInterface::CommunicationThread()
    {
        std::cout << "Communication thread started\n";

        while (!m_isShuttingDown)
        {
            if (m_isConnected)
            {
                // Simulate communication processing
                std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS

                // Occasionally generate test events
                static int eventCounter = 0;
                if (++eventCounter % 240 == 0)
                { // Every 4 seconds at 60 FPS
                    EngineEvent testEvent;
                    testEvent.type = EngineEventType::PERFORMANCE_UPDATE;
                    testEvent.eventID = GenerateEventID();
                    testEvent.message = "Performance metrics updated";
                    testEvent.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch())
                                              .count();

                    std::lock_guard<std::mutex> lock(m_eventMutex);
                    m_incomingEvents.push(testEvent);
                    m_connectionStats.eventsReceived++;
                }
            }
            else
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }

        std::cout << "Communication thread stopped\n";
    }

    void EngineInterface::ProcessIncomingEvents()
    {
        // Drain the queue under lock, then invoke callbacks without holding the lock.
        // This prevents deadlocks if a callback tries to send a command or queue an event.
        std::vector<EngineEvent> pendingEvents;
        {
            std::lock_guard<std::mutex> lock(m_eventMutex);
            while (!m_incomingEvents.empty())
            {
                pendingEvents.push_back(m_incomingEvents.front());
                m_incomingEvents.pop();
            }
        }

        for (const auto& event : pendingEvents)
        {
            auto it = m_eventCallbacks.find(event.type);
            if (it != m_eventCallbacks.end() && it->second)
            {
                it->second(event);
            }
        }
    }

    void EngineInterface::ProcessOutgoingCommands()
    {
        std::lock_guard<std::mutex> lock(m_commandMutex);

        while (!m_outgoingCommands.empty())
        {
            EngineCommand command = m_outgoingCommands.front();
            m_outgoingCommands.pop();

            // Simulate sending command to engine
            std::cout << "Processing command " << command.commandID << " of type "
                      << static_cast<uint32_t>(command.type) << "\n";

            // Simulate network transmission
            m_connectionStats.bytesTransmitted += 1024; // Simulate 1KB per command
        }
    }

    void EngineInterface::HandleConnectionEvents()
    {
        // Handle auto-reconnection if enabled
        if (!m_isConnected && m_autoReconnect)
        {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - m_lastReconnectAttempt).count() >=
                m_reconnectInterval)
            {
                m_lastReconnectAttempt = now;
                Connect(m_commandTimeout);
            }
        }
    }

    // Helper: write a uint32 in little-endian into a byte buffer
    static void WriteU32(std::vector<uint8_t>& buf, uint32_t value)
    {
        buf.push_back(static_cast<uint8_t>(value & 0xFF));
        buf.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    }

    // Helper: write a uint64 in little-endian into a byte buffer
    static void WriteU64(std::vector<uint8_t>& buf, uint64_t value)
    {
        for (int i = 0; i < 8; ++i)
        {
            buf.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    // Helper: write a length-prefixed string into a byte buffer
    static void WriteString(std::vector<uint8_t>& buf, const std::string& str)
    {
        WriteU32(buf, static_cast<uint32_t>(str.size()));
        buf.insert(buf.end(), str.begin(), str.end());
    }

    // Helper: read a uint32 from a byte buffer at the given offset
    static uint32_t ReadU32(const std::vector<uint8_t>& buf, size_t& offset)
    {
        if (offset + 4 > buf.size())
            return 0;
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i)
        {
            value |= static_cast<uint32_t>(buf[offset + i]) << (i * 8);
        }
        offset += 4;
        return value;
    }

    // Helper: read a uint64 from a byte buffer at the given offset
    static uint64_t ReadU64(const std::vector<uint8_t>& buf, size_t& offset)
    {
        if (offset + 8 > buf.size())
            return 0;
        uint64_t value = 0;
        for (int i = 0; i < 8; ++i)
        {
            value |= static_cast<uint64_t>(buf[offset + i]) << (i * 8);
        }
        offset += 8;
        return value;
    }

    // Helper: read a length-prefixed string from a byte buffer
    static std::string ReadString(const std::vector<uint8_t>& buf, size_t& offset)
    {
        uint32_t len = ReadU32(buf, offset);
        if (offset + len > buf.size())
            return "";
        std::string str(buf.begin() + offset, buf.begin() + offset + len);
        offset += len;
        return str;
    }

    bool EngineInterface::SerializeCommand(const EngineCommand& command, std::vector<uint8_t>& buffer)
    {
        // Binary message format:
        //   [4 bytes] magic (0x53504B43 = "SPKC")
        //   [4 bytes] command type (uint32)
        //   [8 bytes] command ID (uint64)
        //   [8 bytes] timestamp (uint64)
        //   [string]  targetObjectID  (length-prefixed)
        //   [string]  componentType   (length-prefixed)
        //   [4 bytes] parameter count
        //   for each parameter:
        //     [string] key
        //     [string] value
        //   [4 bytes] binary data length
        //   [N bytes] binary data payload

        buffer.clear();
        buffer.reserve(512);

        // Magic header
        WriteU32(buffer, 0x53504B43);

        WriteU32(buffer, static_cast<uint32_t>(command.type));
        WriteU64(buffer, command.commandID);
        WriteU64(buffer, command.timestamp);
        WriteString(buffer, command.targetObjectID);
        WriteString(buffer, command.componentType);

        // Parameters
        WriteU32(buffer, static_cast<uint32_t>(command.parameters.size()));
        for (const auto& [key, value] : command.parameters)
        {
            WriteString(buffer, key);
            WriteString(buffer, value);
        }

        // Binary payload
        WriteU32(buffer, static_cast<uint32_t>(command.binaryData.size()));
        buffer.insert(buffer.end(), command.binaryData.begin(), command.binaryData.end());

        return true;
    }

    bool EngineInterface::DeserializeEvent(const std::vector<uint8_t>& buffer, EngineEvent& event)
    {
        // Binary message format (mirrors serialization):
        //   [4 bytes] magic (0x53504B45 = "SPKE")
        //   [4 bytes] event type (uint32)
        //   [8 bytes] event ID (uint64)
        //   [8 bytes] timestamp (uint64)
        //   [4 bytes] severity (int32)
        //   [string]  sourceObjectID
        //   [string]  message
        //   [4 bytes] data count
        //   for each data entry:
        //     [string] key
        //     [string] value
        //   [4 bytes] binary data length
        //   [N bytes] binary data payload

        if (buffer.size() < 4)
            return false;

        size_t offset = 0;
        uint32_t magic = ReadU32(buffer, offset);
        if (magic != 0x53504B45)
        {
            std::cout << "DeserializeEvent: invalid magic number\n";
            return false;
        }

        event.type = static_cast<EngineEventType>(ReadU32(buffer, offset));
        event.eventID = ReadU64(buffer, offset);
        event.timestamp = ReadU64(buffer, offset);
        event.severity = static_cast<int>(ReadU32(buffer, offset));
        event.sourceObjectID = ReadString(buffer, offset);
        event.message = ReadString(buffer, offset);

        uint32_t dataCount = ReadU32(buffer, offset);
        event.data.clear();
        for (uint32_t i = 0; i < dataCount; ++i)
        {
            std::string key = ReadString(buffer, offset);
            std::string value = ReadString(buffer, offset);
            event.data[key] = value;
        }

        uint32_t binaryLen = ReadU32(buffer, offset);
        if (offset + binaryLen <= buffer.size())
        {
            event.binaryData.assign(buffer.begin() + offset, buffer.begin() + offset + binaryLen);
            offset += binaryLen;
        }

        return true;
    }

    bool EngineInterface::CreateCommPipe()
    {
        std::cout << "Creating named pipe: " << m_pipeName << "\n";

#ifdef _WIN32
        std::string fullPipeName = "\\\\.\\pipe\\" + m_pipeName;

        HANDLE hPipe = ::CreateNamedPipeA(fullPipeName.c_str(),
                                          PIPE_ACCESS_DUPLEX,                                    // read/write access
                                          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, // message-type pipe
                                          1,                                                     // max instances
                                          4096,                                                  // output buffer size
                                          4096,                                                  // input buffer size
                                          0,                                                     // default timeout
                                          nullptr                                                // default security
        );

        if (hPipe == INVALID_HANDLE_VALUE)
        {
            std::cerr << "CreateCommPipe failed with error " << GetLastError() << "\n";
            return false;
        }

        m_pipeHandle = static_cast<void*>(hPipe);
        std::cout << "Named pipe created successfully\n";
        return true;
#else
        // Named pipes are a Windows-specific concept; on other platforms a
        // UNIX domain socket or other IPC mechanism would be used instead.
        std::cout << "Named pipes are only supported on Windows\n";
        return false;
#endif
    }

    bool EngineInterface::ConnectToNamedPipe()
    {
        std::cout << "Connecting to named pipe: " << m_pipeName << "\n";

#ifdef _WIN32
        if (!m_pipeHandle)
        {
            std::cerr << "ConnectToNamedPipe: pipe handle is null, create the pipe first\n";
            return false;
        }

        HANDLE hPipe = static_cast<HANDLE>(m_pipeHandle);

        // Wait for a client to connect to the pipe.  ConnectNamedPipe returns
        // TRUE when a new client connects, or FALSE if the client connected
        // between CreateCommPipe and ConnectNamedPipe (in which case
        // GetLastError() == ERROR_PIPE_CONNECTED and we treat it as success).
        BOOL connected = ::ConnectNamedPipe(hPipe, nullptr);
        if (!connected)
        {
            DWORD err = GetLastError();
            if (err == ERROR_PIPE_CONNECTED)
            {
                // Client connected between Create and Connect -- that is fine.
                std::cout << "Client already connected to pipe\n";
                return true;
            }
            std::cerr << "ConnectNamedPipe failed with error " << err << "\n";
            return false;
        }

        std::cout << "Client connected to named pipe successfully\n";
        return true;
#else
        std::cout << "Named pipes are only supported on Windows\n";
        return false;
#endif
    }

    uint64_t EngineInterface::GenerateCommandID()
    {
        return m_nextCommandID++;
    }

    uint64_t EngineInterface::GenerateEventID()
    {
        return m_nextEventID++;
    }

} // namespace SparkEditor