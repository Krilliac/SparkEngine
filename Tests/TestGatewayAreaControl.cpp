#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "TestFramework.h"
#include "GatewayApplication.h"
#include "GatewaySecurity.h"
#include "ScopedLoggerBaseline.h"
#include "Utils/DaemonClient.h"
#include "Utils/DaemonProtocol.h"
#include "Utils/Logger.h"
#include "Engine/Networking/AreaHandoffDispatcher.h"
// Keep the private-member access limited to this test translation unit; the
// production header never exposes a test friend or macro-controlled authority.
// clang-format off
#define private public
#include "GatewayAreaControl.h"
#undef private
// clang-format on

using namespace Spark::Gateway;

namespace
{
    uint64_t ProcessId()
    {
#ifdef _WIN32
        return static_cast<uint64_t>(::GetCurrentProcessId());
#else
        return static_cast<uint64_t>(::getpid());
#endif
    }

    std::string UniqueName(std::string_view prefix)
    {
        static std::atomic<uint64_t> counter{0};
        return std::string(prefix) + "-" + std::to_string(ProcessId()) + "-" +
               std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    }

    uint16_t UniquePort(uint16_t offset)
    {
        return static_cast<uint16_t>(20000 + ((ProcessId() * 17 + offset) % 30000));
    }

    int64_t WallClockMilliseconds()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    std::string LowerHex(const std::vector<uint8_t>& bytes)
    {
        static constexpr char Digits[] = "0123456789abcdef";
        std::string text;
        for (const uint8_t byte : bytes)
        {
            text.push_back(Digits[byte >> 4]);
            text.push_back(Digits[byte & 0x0f]);
        }
        return text;
    }

    // Independent encoder for the documented area-control wire format, so the
    // hostile tests can forge frames the production client would never emit.
    // The timestamp is passed as its wire text so a test can sign values an
    // int64_t cannot carry.
    std::vector<uint8_t> ForgeAreaControlFrameWithTimestampText(const std::vector<uint8_t>& key, AreaControlPhase phase,
                                                                const HandoffCommand& command,
                                                                const std::string& timestampText, uint64_t nonce,
                                                                std::string* macHex = nullptr)
    {
        const std::string body = std::to_string(GatewayProtocolMajor) + "\n" + std::to_string(GatewayProtocolMinor) +
                                 "\n" + timestampText + "\n" + std::to_string(nonce) + "\n" +
                                 std::to_string(static_cast<unsigned int>(phase)) + "\n" +
                                 std::to_string(command.epoch) + "\n" + std::to_string(command.sourceArea) + "\n" +
                                 std::to_string(command.targetArea) + "\n" + command.sessionId;
        const std::string mac = LowerHex(ComputeGatewayMac(key, body));
        if (macHex)
            *macHex = mac;
        const std::string frame = body + "\n" + mac;
        return {frame.begin(), frame.end()};
    }

    std::vector<uint8_t> ForgeAreaControlFrame(const std::vector<uint8_t>& key, AreaControlPhase phase,
                                               const HandoffCommand& command, int64_t timestamp, uint64_t nonce,
                                               std::string* macHex = nullptr)
    {
        return ForgeAreaControlFrameWithTimestampText(key, phase, command, std::to_string(timestamp), nonce, macHex);
    }

    std::string LocalAreaControlAddress(const std::string& endpoint)
    {
#ifdef _WIN32
        return endpoint;
#else
        return "/tmp/" + endpoint + ".sock";
#endif
    }

    // Sends one frame with an arbitrary service id and message type. A reply the
    // client cannot accept (for example one addressed from another service) is
    // reported as Unavailable, never as a service decision.
    HandoffOperationResult SendRawFrame(const std::string& endpoint, Spark::Daemon::ServiceId service,
                                        uint16_t messageType, const std::vector<uint8_t>& payload)
    {
        Spark::Daemon::DaemonClient client;
        if (!client.Connect(LocalAreaControlAddress(endpoint)))
            return HandoffOperationResult::Unavailable;
        auto response = client.Request(service, messageType, payload);
        if (!response || response->payload.size() != 1)
            return HandoffOperationResult::Unavailable;
        return static_cast<HandoffOperationResult>(response->payload[0]);
    }

    HandoffOperationResult SendRawAreaControlFrame(const std::string& endpoint, AreaControlPhase phase,
                                                   const std::vector<uint8_t>& payload)
    {
        return SendRawFrame(endpoint, Spark::Daemon::ServiceId::Orchestration, static_cast<uint16_t>(phase), payload);
    }

    // Raw byte stream to the service endpoint, below the DaemonClient framing,
    // so a test can send headers and truncated bodies no client would emit.
    class RawAreaControlConnection
    {
      public:
        explicit RawAreaControlConnection(const std::string& endpoint)
        {
#ifdef _WIN32
            const std::wstring pipe = L"\\\\.\\pipe\\" + std::wstring(endpoint.begin(), endpoint.end());
            for (int retry = 0; retry < 50 && m_handle == INVALID_HANDLE_VALUE; ++retry)
            {
                m_handle =
                    CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
                if (m_handle == INVALID_HANDLE_VALUE)
                    WaitNamedPipeW(pipe.c_str(), 20);
            }
#else
            const std::string socketPath = LocalAreaControlAddress(endpoint);
            m_socket = ::socket(AF_UNIX, SOCK_STREAM, 0);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if (m_socket >= 0 && socketPath.size() < sizeof(address.sun_path))
            {
                std::memcpy(address.sun_path, socketPath.data(), socketPath.size());
                if (::connect(m_socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
                {
                    ::close(m_socket);
                    m_socket = -1;
                }
            }
#endif
        }

        RawAreaControlConnection(const RawAreaControlConnection&) = delete;
        RawAreaControlConnection& operator=(const RawAreaControlConnection&) = delete;

        ~RawAreaControlConnection()
        {
#ifdef _WIN32
            if (m_handle != INVALID_HANDLE_VALUE)
                CloseHandle(m_handle);
#else
            if (m_socket >= 0)
                ::close(m_socket);
#endif
        }

        [[nodiscard]] bool IsOpen() const
        {
#ifdef _WIN32
            return m_handle != INVALID_HANDLE_VALUE;
#else
            return m_socket >= 0;
#endif
        }

        [[nodiscard]] bool Write(const std::vector<uint8_t>& bytes) const
        {
#ifdef _WIN32
            DWORD written = 0;
            return WriteFile(m_handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) != 0 &&
                   written == bytes.size();
#else
            return ::send(m_socket, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size());
#endif
        }

      private:
#ifdef _WIN32
        HANDLE m_handle = INVALID_HANDLE_VALUE;
#else
        int m_socket = -1;
#endif
    };

    std::vector<uint8_t> EncodeRawHeader(uint32_t payloadSize, AreaControlPhase phase)
    {
        const Spark::Daemon::FrameHeader header{
            payloadSize, static_cast<uint16_t>(Spark::Daemon::ServiceId::Orchestration), static_cast<uint16_t>(phase)};
        uint8_t encoded[Spark::Daemon::kFrameHeaderSize];
        Spark::Daemon::EncodeFrameHeader(header, encoded);
        return {std::begin(encoded), std::end(encoded)};
    }

    // Frames with no reply (dropped before a whole frame arrived) are only
    // observable through the audit counter, so poll it against a deadline.
    bool WaitForAuditCount(const LocalAreaControlService& service, AreaControlAuditReason reason, uint64_t expected)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (service.GetAuditCount(reason) < expected && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return service.GetAuditCount(reason) == expected;
    }

    uint64_t TotalAuditCount(const LocalAreaControlService& service)
    {
        uint64_t total = 0;
        for (size_t index = 0; index < static_cast<size_t>(AreaControlAuditReason::Count); ++index)
            total += service.GetAuditCount(static_cast<AreaControlAuditReason>(index));
        return total;
    }
} // namespace

TEST(GatewayOptions_GenerateKeyModeIsStandalone)
{
    const std::vector<std::string_view> generate{"--generate-key", "private/gateway.key"};
    const GatewayParseResult parsed = ParseGatewayOptions(generate);
    ASSERT_TRUE(parsed.options.has_value());
    EXPECT_EQ(parsed.options->generateKeyFile, std::filesystem::path("private/gateway.key"));
    EXPECT_TRUE(parsed.options->configPath.empty());

    const std::vector<std::string_view> mixed{"--generate-key", "private/gateway.key", "--config", "gateway.ini"};
    const GatewayParseResult rejected = ParseGatewayOptions(mixed);
    EXPECT_FALSE(rejected.options.has_value());
    EXPECT_TRUE(rejected.error.find("mutually exclusive") != std::string::npos);
}

TEST(GatewayAreaControl_LiveLoopbackIsIdempotentAndPersistsEpochFence)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-area-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x3c);
    const uint16_t controlPort = UniquePort(1);

    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.interServerPort = controlPort;
    LocalAreaControlPlane client(key);
    client.RegisterEndpoint(7, area);
    // Match the deterministic endpoint derived by RegisterEndpoint.
    LocalAreaControlService service("spark-area-control-" + std::to_string(controlPort), key, state);
    EXPECT_TRUE(service.Start());
    EXPECT_TRUE(client.IsReady());
    LocalAreaControlPlane wrongKey(std::vector<uint8_t>(32, 0x4d));
    wrongKey.RegisterEndpoint(7, area);
    EXPECT_FALSE(wrongKey.IsReady());

    LocalAreaControlPlane secondClient(key);
    secondClient.RegisterEndpoint(7, area);
    EXPECT_EQ(static_cast<int>(secondClient.Prepare({"second-client", 1, 7, 7})),
              static_cast<int>(HandoffOperationResult::Applied));

    HandoffCommand command{"session-live", 4, 7, 7};
    EXPECT_EQ(static_cast<int>(client.Prepare(command)), static_cast<int>(HandoffOperationResult::Applied));
    EXPECT_EQ(static_cast<int>(client.Prepare(command)), static_cast<int>(HandoffOperationResult::Duplicate));
    EXPECT_EQ(static_cast<int>(client.Transfer(command)), static_cast<int>(HandoffOperationResult::Applied));
    EXPECT_EQ(static_cast<int>(client.Commit(command)), static_cast<int>(HandoffOperationResult::Applied));
    EXPECT_EQ(static_cast<int>(client.Acknowledge(command)), static_cast<int>(HandoffOperationResult::Applied));
    service.Stop();
    EXPECT_FALSE(client.IsReady());

    LocalAreaControlService restarted("spark-area-control-" + std::to_string(controlPort), key, state);
    EXPECT_TRUE(restarted.Start());
    HandoffCommand stale{"session-live", 3, 7, 7};
    EXPECT_TRUE(client.Prepare(stale) == HandoffOperationResult::Rejected);
    HandoffCommand next{"session-live", 5, 7, 7};
    EXPECT_TRUE(client.Prepare(next) == HandoffOperationResult::Applied);
    restarted.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsNewNoncesWhenReplayLedgerIsFull)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-replay-limit") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x43);
    const uint16_t controlPort = UniquePort(6);
    const std::string endpoint = "spark-area-control-" + std::to_string(controlPort);

    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    for (uint64_t nonce = 1; nonce <= GatewayMaximumReplayEntries; ++nonce)
        service.m_seenNonces.emplace(nonce, now);
    EXPECT_EQ(service.m_seenNonces.size(), GatewayMaximumReplayEntries);

    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.interServerPort = controlPort;
    LocalAreaControlPlane client(key);
    client.RegisterEndpoint(7, area);

    EXPECT_FALSE(client.IsEndpointReady(7));
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::LedgerFull), 1u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsWrongKeyMac)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-wrong-mac") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x52);
    const std::vector<uint8_t> attackerKey(32, 0x53);
    const std::string endpoint = UniqueName("spark-area-control-wrong-mac");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    const HandoffCommand command{"wrong-key-session", 1, 7, 7};
    const auto forged =
        ForgeAreaControlFrame(attackerKey, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x1001);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, forged) ==
                HandoffOperationResult::Rejected);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::MacInvalid), 1u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
    EXPECT_EQ(TotalAuditCount(service), 1u);
    // The forged frame never reached the epoch fence, so nothing was persisted.
    EXPECT_FALSE(std::filesystem::exists(state));

    // The same command under the real key is a first Prepare (Applied), not a
    // Duplicate, which proves the forged frame left no fence behind.
    const auto genuine =
        ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x1002);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, genuine) ==
                HandoffOperationResult::Applied);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 1u);

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsStaleAndFutureTimestamp)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-timestamp") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x54);
    const std::string endpoint = UniqueName("spark-area-control-timestamp");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    const HandoffCommand command{"timestamp-session", 1, 7, 7};
    const int64_t now = WallClockMilliseconds();
    const auto stale = ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, now - 61000, 0x2001);
    const auto future = ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, now + 61000, 0x2002);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, stale) ==
                HandoffOperationResult::Rejected);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, future) ==
                HandoffOperationResult::Rejected);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::TimestampWindow), 2u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
    EXPECT_FALSE(std::filesystem::exists(state));

    const auto fresh = ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x2003);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, fresh) == HandoffOperationResult::Applied);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 1u);

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsReplayedNonce)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-replay") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x55);
    const std::string endpoint = UniqueName("spark-area-control-replay");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    const HandoffCommand command{"replay-session", 1, 7, 7};
    const auto frame = ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x3001);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, frame) == HandoffOperationResult::Applied);
    // Without the nonce ledger the byte-identical frame would reach the fence
    // and come back Duplicate; the ledger must reject it before that.
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, frame) ==
                HandoffOperationResult::Rejected);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 1u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Replay), 1u);
    EXPECT_EQ(TotalAuditCount(service), 2u);

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_AuditRecordOmitsSecrets)
{
    // Restores TestMain's full logger baseline (level, category mask, sinks)
    // on entry and on every exit path, so a neighbour's leaked filter cannot
    // hide the Warn/Network record and this test cannot leak its sink.
    ScopedLoggerBaseline loggerBaseline;
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-audit-log") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x56);
    const std::vector<uint8_t> attackerKey(32, 0x57);
    const std::string endpoint = UniqueName("spark-area-control-audit-log");

    // The sink owns the capture state, so it stays valid for as long as the
    // sink is installed, including until the baseline removes it on exit.
    struct CapturedRecords
    {
        std::mutex mutex;
        std::vector<std::string> records;
    };
    const auto captured = std::make_shared<CapturedRecords>();
    auto& logger = Spark::Logger::Get();
    logger.ClearSinks();
    logger.AddSink(std::make_unique<Spark::CallbackSink>(
        [captured](const Spark::LogMessage& message)
        {
            if (message.message.find("GatewayAreaControl:") == std::string::npos)
                return;
            std::lock_guard lock(captured->mutex);
            captured->records.push_back(message.message);
        }));

    {
        LocalAreaControlService service(endpoint, key, state);
        ASSERT_TRUE(service.Start());
        // A carriage return and spaces in the claimed session must not reach
        // the log verbatim, and the long tail must be truncated.
        const HandoffCommand command{"audit session\rINJECTED-long-session-tail", 9, 7, 7};
        const uint64_t nonce = 987654321987ull;
        std::string forgedMacHex;
        const auto forged = ForgeAreaControlFrame(attackerKey, AreaControlPhase::Commit, command,
                                                  WallClockMilliseconds(), nonce, &forgedMacHex);
        EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Commit, forged) ==
                    HandoffOperationResult::Rejected);
        service.Stop();
        logger.FlushAll();

        std::lock_guard lock(captured->mutex);
        ASSERT_EQ(captured->records.size(), static_cast<size_t>(1));
        const std::string& record = captured->records.front();
        EXPECT_STR_CONTAINS(record, "GatewayAreaControl: reason=mac_invalid phase=3 epoch=9 "
                                    "session=audit?session?IN~ outcome=rejected");
        EXPECT_TRUE(record.find(forgedMacHex) == std::string::npos);
        EXPECT_TRUE(record.find(LowerHex(key)) == std::string::npos);
        EXPECT_TRUE(record.find(LowerHex(attackerKey)) == std::string::npos);
        EXPECT_TRUE(record.find(std::to_string(nonce)) == std::string::npos);
        EXPECT_TRUE(record.find('\r') == std::string::npos);
        EXPECT_TRUE(record.find("long-session-tail") == std::string::npos);
    }
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsOversizeFrame)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-oversize") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x58);
    const std::string endpoint = UniqueName("spark-area-control-oversize");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    // A genuine frame padded one byte past the body limit: the size check must
    // reject it before decoding, so it is Oversize and not DecodeFailed.
    const HandoffCommand command{"oversize-session", 1, 7, 7};
    auto oversized = ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x4001);
    oversized.resize(GatewayMaximumBodySize + 1, static_cast<uint8_t>('a'));
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, oversized) ==
                HandoffOperationResult::Rejected);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Oversize), 1u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::DecodeFailed), 0u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
    EXPECT_EQ(TotalAuditCount(service), 1u);
    EXPECT_FALSE(std::filesystem::exists(state));

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsWrongServiceId)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-wrong-service") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x59);
    const std::string endpoint = UniqueName("spark-area-control-wrong-service");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    // A correctly signed frame addressed to another daemon service is refused
    // before decoding. The reply comes back under the Orchestration service id,
    // which the client rejects as a mismatch, so only the audit counter can say
    // what the service decided.
    const HandoffCommand command{"wrong-service-session", 1, 7, 7};
    const auto frame = ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x5001);
    EXPECT_TRUE(SendRawFrame(endpoint, Spark::Daemon::ServiceId::Asset,
                             static_cast<uint16_t>(AreaControlPhase::Prepare),
                             frame) != HandoffOperationResult::Applied);
    EXPECT_TRUE(WaitForAuditCount(service, AreaControlAuditReason::WrongService, 1u));
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
    EXPECT_EQ(TotalAuditCount(service), 1u);
    EXPECT_FALSE(std::filesystem::exists(state));

    // The rejection consumed no nonce and set no fence: the byte-identical
    // frame on the right service is a first Prepare.
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, frame) == HandoffOperationResult::Applied);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 1u);

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsMalformedFrames)
{
    ScopedLoggerBaseline loggerBaseline;
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-malformed") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x5a);
    const std::string endpoint = UniqueName("spark-area-control-malformed");

    struct CapturedRecords
    {
        std::mutex mutex;
        std::vector<std::string> records;
    };
    const auto captured = std::make_shared<CapturedRecords>();
    auto& logger = Spark::Logger::Get();
    logger.ClearSinks();
    logger.AddSink(std::make_unique<Spark::CallbackSink>(
        [captured](const Spark::LogMessage& message)
        {
            if (message.message.find("GatewayAreaControl:") == std::string::npos)
                return;
            std::lock_guard lock(captured->mutex);
            captured->records.push_back(message.message);
        }));

    // Every case starts from the fields of one genuine, correctly signed frame
    // and breaks exactly one rule of the wire format.
    const HandoffCommand command{"malformed-session", 1, 7, 7};
    std::string genuineMacHex;
    const auto genuine =
        ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x6001, &genuineMacHex);
    std::vector<std::string> genuineFields;
    {
        const std::string text(genuine.begin(), genuine.end());
        size_t begin = 0;
        for (size_t end = text.find('\n'); end != std::string::npos; end = text.find('\n', begin))
        {
            genuineFields.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        genuineFields.push_back(text.substr(begin));
    }
    ASSERT_EQ(genuineFields.size(), static_cast<size_t>(10));

    const auto join = [](const std::vector<std::string>& fields)
    {
        std::string text;
        for (size_t index = 0; index < fields.size(); ++index)
            text += (index == 0 ? "" : "\n") + fields[index];
        return std::vector<uint8_t>(text.begin(), text.end());
    };
    const auto mutate = [&](size_t field, std::string value)
    {
        std::vector<std::string> fields = genuineFields;
        fields[field] = std::move(value);
        return join(fields);
    };

    std::vector<std::vector<uint8_t>> cases;
    cases.push_back({});                                                                               // empty payload
    cases.push_back(join(std::vector<std::string>(genuineFields.begin(), genuineFields.begin() + 9))); // 9 fields
    std::vector<std::string> elevenFields = genuineFields;
    elevenFields.push_back("x");
    cases.push_back(join(elevenFields));                                  // 11 fields
    cases.push_back(mutate(0, std::to_string(GatewayProtocolMajor + 1))); // wrong major
    cases.push_back(mutate(1, std::to_string(GatewayProtocolMinor + 1))); // newer minor
    cases.push_back(mutate(2, "not-a-timestamp"));                        // non-numeric timestamp
    cases.push_back(mutate(2, "-5"));                                     // signed timestamp
    cases.push_back(mutate(3, ""));                                       // empty nonce
    cases.push_back(mutate(4, "0"));                                      // phase below Prepare
    cases.push_back(mutate(4, std::to_string(static_cast<unsigned int>(AreaControlPhase::Probe) + 1))); // above Probe
    cases.push_back(mutate(6, "4294967296"));                // source beyond AreaID
    cases.push_back(mutate(7, "4294967296"));                // target beyond AreaID
    cases.push_back(mutate(8, ""));                          // empty session
    cases.push_back(mutate(8, std::string(129, 's')));       // 129-character session
    cases.push_back(mutate(9, genuineMacHex.substr(0, 63))); // 63-character MAC
    cases.push_back(mutate(9, genuineMacHex + "0"));         // 65-character MAC
    cases.push_back(mutate(9, std::string(64, 'g')));        // non-hex MAC
    std::vector<uint8_t> trailingNewline = genuine;
    trailingNewline.push_back('\n');
    cases.push_back(trailingNewline); // trailing newline

    {
        LocalAreaControlService service(endpoint, key, state);
        ASSERT_TRUE(service.Start());
        for (const auto& payload : cases)
            EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, payload) ==
                        HandoffOperationResult::Rejected);
        EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::DecodeFailed), static_cast<uint64_t>(cases.size()));
        EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
        EXPECT_EQ(TotalAuditCount(service), static_cast<uint64_t>(cases.size()));
        EXPECT_FALSE(std::filesystem::exists(state));

        // The genuine frame the cases were derived from is still accepted, so
        // each rejection above is the single broken rule, not the base frame.
        EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, genuine) ==
                    HandoffOperationResult::Applied);
        service.Stop();
    }
    logger.FlushAll();

    std::lock_guard lock(captured->mutex);
    ASSERT_EQ(captured->records.size(), cases.size() + 1);
    for (size_t index = 0; index < cases.size(); ++index)
    {
        const std::string& record = captured->records[index];
        EXPECT_STR_CONTAINS(record, "reason=decode_failed");
        EXPECT_STR_CONTAINS(record, "session=- outcome=rejected");
        EXPECT_TRUE(record.find(genuineMacHex) == std::string::npos);
        EXPECT_TRUE(record.find(LowerHex(key)) == std::string::npos);
        for (const char character : record)
            EXPECT_TRUE(static_cast<unsigned char>(character) >= 0x20 && character != 0x7f);
    }
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsPhaseMismatch)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-phase-mismatch") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x5b);
    const std::string endpoint = UniqueName("spark-area-control-phase-mismatch");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    // A correctly signed Prepare delivered under the Commit message type: the
    // frame's routing phase must match the signed phase, and the mismatch is
    // classified before the MAC, freshness or nonce ledger are consulted.
    const HandoffCommand command{"phase-mismatch-session", 1, 7, 7};
    const auto frame = ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x7001);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Commit, frame) == HandoffOperationResult::Rejected);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::PhaseMismatch), 1u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::MacInvalid), 0u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
    EXPECT_EQ(TotalAuditCount(service), 1u);
    EXPECT_FALSE(std::filesystem::exists(state));

    // The mismatched delivery consumed no nonce: the same bytes under the
    // signed phase are a first Prepare.
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, frame) == HandoffOperationResult::Applied);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 1u);

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsOutOfRangeTimestamp)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-timestamp-range") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x5c);
    const std::string endpoint = UniqueName("spark-area-control-timestamp-range");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    // Correctly signed timestamps that parse as uint64 but wrap negative when
    // narrowed to int64_t must land outside the freshness window, never inside.
    const HandoffCommand command{"timestamp-range-session", 1, 7, 7};
    const auto wrapsToMinusOne =
        ForgeAreaControlFrameWithTimestampText(key, AreaControlPhase::Prepare, command, "18446744073709551615", 0x8001);
    const auto wrapsToMinimum =
        ForgeAreaControlFrameWithTimestampText(key, AreaControlPhase::Prepare, command, "9223372036854775808", 0x8002);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, wrapsToMinusOne) ==
                HandoffOperationResult::Rejected);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, wrapsToMinimum) ==
                HandoffOperationResult::Rejected);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::TimestampWindow), 2u);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
    EXPECT_EQ(TotalAuditCount(service), 2u);
    EXPECT_FALSE(std::filesystem::exists(state));

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RejectsTruncatedAndOverlongHeaderFrames)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-truncated") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x5d);
    const std::string endpoint = UniqueName("spark-area-control-truncated");
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    // A header claiming more than the daemon payload bound is dropped without
    // allocating or waiting for the body. The connection stays open until the
    // audit lands so the service cannot see a pre-accept disconnect instead.
    {
        RawAreaControlConnection connection(endpoint);
        ASSERT_TRUE(connection.IsOpen());
        EXPECT_TRUE(connection.Write(EncodeRawHeader(Spark::Daemon::kMaxPayloadSize + 1, AreaControlPhase::Prepare)));
        EXPECT_TRUE(WaitForAuditCount(service, AreaControlAuditReason::Incomplete, 1u));
    }

    // A header followed by a short body: the service gives up at its I/O
    // deadline and records the frame as incomplete.
    {
        RawAreaControlConnection connection(endpoint);
        ASSERT_TRUE(connection.IsOpen());
        std::vector<uint8_t> truncated = EncodeRawHeader(100, AreaControlPhase::Prepare);
        truncated.insert(truncated.end(), 10, static_cast<uint8_t>('1'));
        EXPECT_TRUE(connection.Write(truncated));
        EXPECT_TRUE(WaitForAuditCount(service, AreaControlAuditReason::Incomplete, 2u));
    }
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 0u);
    EXPECT_EQ(TotalAuditCount(service), 2u);
    EXPECT_FALSE(std::filesystem::exists(state));

    // The service survives both and still applies a genuine frame.
    const HandoffCommand command{"truncated-session", 1, 7, 7};
    const auto genuine =
        ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x9001);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, genuine) ==
                HandoffOperationResult::Applied);
    EXPECT_EQ(service.GetAuditCount(AreaControlAuditReason::Accepted), 1u);

    service.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_RetriesTransientStateReplaceSharingViolation)
{
#ifndef _WIN32
    SKIP_TEST("Windows atomic-replace sharing regression");
#else
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-state-sharing") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    {
        std::ofstream initial(state);
        ASSERT_TRUE(initial.good());
    }

    const std::vector<uint8_t> key(32, 0x4e);
    const uint16_t controlPort = UniquePort(62);
    const std::string endpoint = "spark-area-control-" + std::to_string(controlPort);
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    HANDLE held = CreateFileW(state.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_TRUE(held != INVALID_HANDLE_VALUE);
    std::jthread releaseHeld(
        [held]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            CloseHandle(held);
        });

    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.interServerPort = controlPort;
    LocalAreaControlPlane client(key);
    client.RegisterEndpoint(7, area);
    const HandoffCommand command{"sharing-retry", 1, 7, 7};
    EXPECT_TRUE(client.Prepare(command) == HandoffOperationResult::Applied);
    releaseHeld.join();
    service.Stop();

    LocalAreaControlService restarted(endpoint, key, state);
    ASSERT_TRUE(restarted.Start());
    EXPECT_TRUE(client.Prepare(command) == HandoffOperationResult::Duplicate);
    restarted.Stop();
    std::filesystem::remove(state, error);
#endif
}

TEST(GatewayAreaControl_RecoversWhenClientDisconnectsBeforeAccept)
{
#ifndef _WIN32
    SKIP_TEST("Windows named-pipe recovery regression");
#else
    const auto state =
        std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-preaccept-disconnect") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x5e);
    const uint16_t controlPort = UniquePort(61);
    const std::string endpoint = "spark-area-control-" + std::to_string(controlPort);
    LocalAreaControlService service(endpoint, key, state);
    ASSERT_TRUE(service.Start());

    const std::wstring pipe = L"\\\\.\\pipe\\" + std::wstring(endpoint.begin(), endpoint.end());
    for (int attempt = 0; attempt < 32; ++attempt)
    {
        HANDLE rawClient = INVALID_HANDLE_VALUE;
        for (int retry = 0; retry < 50 && rawClient == INVALID_HANDLE_VALUE; ++retry)
        {
            rawClient = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (rawClient == INVALID_HANDLE_VALUE)
            {
                WaitNamedPipeW(pipe.c_str(), 20);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        ASSERT_TRUE(rawClient != INVALID_HANDLE_VALUE);
        CloseHandle(rawClient);
    }

    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.interServerPort = controlPort;
    LocalAreaControlPlane client(key);
    client.RegisterEndpoint(7, area);
    EXPECT_TRUE(client.IsReady());
    const HandoffCommand command{"preaccept-recovery", 1, 7, 7};
    EXPECT_TRUE(client.Prepare(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(client.Transfer(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(client.Commit(command) == HandoffOperationResult::Applied);
    service.Stop();
    std::filesystem::remove(state, error);
#endif
}

TEST(GatewayAreaControl_RequiresExactIPv4LoopbackHost)
{
    for (const std::string host : {"localhost", "::1", "203.0.113.10"})
    {
        LocalAreaControlPlane control(std::vector<uint8_t>(32, 0x65));
        AreaEndpoint area;
        area.host = host;
        area.area.interServerPort = UniquePort(31);
        control.RegisterEndpoint(31, area);
        EXPECT_FALSE(control.IsReady());
        EXPECT_FALSE(control.IsEndpointReady(31));
    }
}

TEST(GatewayCoordinator_RejectsNonCanonicalLoopbackAreaBeforeRegistration)
{
    const std::vector<uint8_t> key(32, 0x66);
    LocalAreaControlPlane control(key);
    KeyFileAuthenticator authenticator(key);
    Spark::Net::WorldServer world;
    Spark::Net::WorldServerConfig worldConfig;
    ASSERT_TRUE(world.Start(worldConfig));

    GatewayCoordinator coordinator(world, authenticator, control);
    AreaEndpoint area;
    area.host = "localhost";
    area.area.areaName = "NonCanonicalLoopback";
    area.area.port = UniquePort(32);
    area.area.interServerPort = UniquePort(33);
    area.area.maxClients = 8;
    EXPECT_FALSE(coordinator.RegisterAreas({area}));

    world.Stop();
}

TEST(GatewayIngress_LiveLoopbackAuthenticatesAndRoutes)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-ingress-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x71);

    const uint16_t controlPort = UniquePort(2);
    const uint16_t gamePort = UniquePort(3);
    const std::string ingressEndpoint = UniqueName("spark-gateway-ingress");
    LocalAreaControlService areaService("spark-area-control-" + std::to_string(controlPort), key, state);
    EXPECT_TRUE(areaService.Start());
    LocalAreaControlPlane control(key);
    KeyFileAuthenticator authenticator(key);
    Spark::Net::WorldServer world;
    Spark::Net::WorldServerConfig worldConfig;
    EXPECT_TRUE(world.Start(worldConfig));
    GatewayCoordinator coordinator(world, authenticator, control);
    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.areaName = "Loopback";
    area.area.port = gamePort;
    area.area.interServerPort = controlPort;
    area.area.maxClients = 8;
    EXPECT_TRUE(coordinator.RegisterAreas({area}));

    LocalGatewayIngressService ingress(ingressEndpoint, coordinator);
    EXPECT_TRUE(ingress.Start());
    AdmissionRequest request;
    request.clientId = 88;
    request.sessionId = "live-\"quoted\\session";
    request.playerName = "Loopback \"Player\" \\";
    const int64_t now =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    request.credential = authenticator.CreateCredential(request, now, 9911);
    const RouteResult routed = LocalGatewayIngressClient(ingressEndpoint).Admit(request);
    EXPECT_TRUE(routed.accepted);
    EXPECT_EQ(routed.port, gamePort);

    ingress.Stop();
    areaService.Stop();
    world.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayIngress_RejectsOversizedCredentialBeforeConnecting)
{
    AdmissionRequest request;
    request.clientId = 88;
    request.sessionId = "oversized-admission";
    request.playerName = "Player";
    request.credential.assign(GatewayMaximumCredentialSize + 1, 'x');

    const RouteResult result = LocalGatewayIngressClient("unused-oversized-ingress").Admit(request);
    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(result.failure == RouteFailure::InvalidRequest);
    EXPECT_EQ(result.reason, std::string("Admission exceeds local ingress frame bounds"));
}

TEST(GatewayAreaControl_LiveTwoAreaHandoffReachesSourceAndTarget)
{
    const auto temp = std::filesystem::temp_directory_path();
    const auto sourceState = temp / (UniqueName("spark-gateway-source-state") + ".txt");
    const auto targetState = temp / (UniqueName("spark-gateway-target-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(sourceState, error);
    std::filesystem::remove(targetState, error);
    const std::vector<uint8_t> key(32, 0x19);
    const uint16_t sourcePort = UniquePort(4);
    const uint16_t targetPort = UniquePort(5);
    LocalAreaControlService source("spark-area-control-" + std::to_string(sourcePort), key, sourceState);
    LocalAreaControlService target("spark-area-control-" + std::to_string(targetPort), key, targetState);
    EXPECT_TRUE(source.Start());
    EXPECT_TRUE(target.Start());

    LocalAreaControlPlane control(key);
    AreaEndpoint sourceEndpoint;
    sourceEndpoint.host = "127.0.0.1";
    sourceEndpoint.area.interServerPort = sourcePort;
    AreaEndpoint targetEndpoint;
    targetEndpoint.host = "127.0.0.1";
    targetEndpoint.area.interServerPort = targetPort;
    control.RegisterEndpoint(1, sourceEndpoint);
    control.RegisterEndpoint(2, targetEndpoint);
    HandoffCommand command{"two-area-session", 1, 1, 2};
    EXPECT_TRUE(control.Prepare(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Transfer(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Commit(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Acknowledge(command) == HandoffOperationResult::Applied);
    EXPECT_TRUE(control.Acknowledge(command) == HandoffOperationResult::Duplicate);

    source.Stop();
    target.Stop();
    std::filesystem::remove(sourceState, error);
    std::filesystem::remove(targetState, error);
}

namespace
{
    // Records which phases reached the game-thread participant through the production dispatcher.
    class RecordingHandoffParticipant final : public Spark::Net::IAreaHandoffParticipant
    {
      public:
        Spark::Net::HandoffResult Prepare(const Spark::Net::HandoffRequest& request) override
        {
            return Record(Spark::Net::HandoffPhase::Prepare, request);
        }
        Spark::Net::HandoffResult Transfer(const Spark::Net::HandoffRequest& request) override
        {
            return Record(Spark::Net::HandoffPhase::Transfer, request);
        }
        Spark::Net::HandoffResult Commit(const Spark::Net::HandoffRequest& request) override
        {
            return Record(Spark::Net::HandoffPhase::Commit, request);
        }
        Spark::Net::HandoffResult Acknowledge(const Spark::Net::HandoffRequest& request) override
        {
            return Record(Spark::Net::HandoffPhase::Acknowledge, request);
        }
        Spark::Net::HandoffResult Abort(const Spark::Net::HandoffRequest& request) override
        {
            return Record(Spark::Net::HandoffPhase::Abort, request);
        }

        size_t Count() const
        {
            std::lock_guard lock(m_mutex);
            return m_calls.size();
        }

        std::pair<Spark::Net::HandoffPhase, uint64_t> Last() const
        {
            std::lock_guard lock(m_mutex);
            return m_calls.back();
        }

      private:
        Spark::Net::HandoffResult Record(Spark::Net::HandoffPhase phase, const Spark::Net::HandoffRequest& request)
        {
            std::lock_guard lock(m_mutex);
            m_calls.emplace_back(phase, request.epoch);
            return Spark::Net::HandoffResult::Applied;
        }

        mutable std::mutex m_mutex;
        std::vector<std::pair<Spark::Net::HandoffPhase, uint64_t>> m_calls;
    };
} // namespace

// TF-120: only a MAC-verified, fresh, never-seen frame whose epoch and phase pass the persisted fence reaches the
// game-thread participant. Forged, replayed, stale-epoch, out-of-order and retargeted frames stop at the service.
TEST(GatewayAreaControl_DispatchesOnlyAuthenticatedFencedPhasesToParticipant)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-dispatch") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x61);
    const std::vector<uint8_t> attackerKey(32, 0x62);
    const std::string endpoint = UniqueName("spark-area-control-dispatch");

    Spark::Net::AreaHandoffDispatcher dispatcher;
    RecordingHandoffParticipant participant;
    std::atomic<bool> pumping{true};
    std::thread gameThread(
        [&]
        {
            while (pumping.load())
            {
                dispatcher.Pump();
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });

    LocalAreaControlService service(endpoint, key, state);
    service.SetHandoffDispatcher(&dispatcher);
    ASSERT_TRUE(service.Start());
    uint64_t nonce = 0x9000;
    const auto send = [&](const std::vector<uint8_t>& signingKey, AreaControlPhase phase, const HandoffCommand& command)
    {
        const auto frame = ForgeAreaControlFrame(signingKey, phase, command, WallClockMilliseconds(), ++nonce);
        return SendRawAreaControlFrame(endpoint, phase, frame);
    };

    // With the dispatcher attached but no participant bound, the service reports itself unavailable.
    const HandoffCommand command{"tf/42", 3, 1, 2};
    EXPECT_TRUE(send(key, AreaControlPhase::Prepare, command) == HandoffOperationResult::Unavailable);
    EXPECT_EQ(participant.Count(), size_t{0});
    dispatcher.SetParticipant(&participant);

    EXPECT_TRUE(send(attackerKey, AreaControlPhase::Prepare, command) == HandoffOperationResult::Rejected);
    EXPECT_EQ(participant.Count(), size_t{0});

    const auto prepare =
        ForgeAreaControlFrame(key, AreaControlPhase::Prepare, command, WallClockMilliseconds(), 0x8001);
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, prepare) ==
                HandoffOperationResult::Applied);
    EXPECT_EQ(participant.Count(), size_t{1});
    EXPECT_TRUE(SendRawAreaControlFrame(endpoint, AreaControlPhase::Prepare, prepare) ==
                HandoffOperationResult::Rejected);                                                 // replayed nonce
    EXPECT_TRUE(send(key, AreaControlPhase::Commit, command) == HandoffOperationResult::Rejected); // skips Transfer
    EXPECT_TRUE(send(key, AreaControlPhase::Transfer, HandoffCommand{"tf/42", 3, 1, 9}) ==
                HandoffOperationResult::Rejected); // same epoch, different target
    EXPECT_TRUE(send(key, AreaControlPhase::Prepare, HandoffCommand{"tf/42", 2, 1, 2}) ==
                HandoffOperationResult::Rejected); // stale epoch
    EXPECT_TRUE(send(key, AreaControlPhase::Prepare, HandoffCommand{"tf/42", 4, 1, 2}) ==
                HandoffOperationResult::Rejected); // newer epoch before this one resolved
    EXPECT_EQ(participant.Count(), size_t{1});

    // A retried phase is redelivered so the participant can answer idempotently; the next phase advances.
    EXPECT_TRUE(send(key, AreaControlPhase::Prepare, command) == HandoffOperationResult::Duplicate);
    EXPECT_EQ(participant.Count(), size_t{2});
    EXPECT_TRUE(send(key, AreaControlPhase::Transfer, command) == HandoffOperationResult::Applied);
    EXPECT_EQ(participant.Count(), size_t{3});
    EXPECT_TRUE(participant.Last().first == Spark::Net::HandoffPhase::Transfer);
    EXPECT_EQ(participant.Last().second, uint64_t{3});

    service.Stop();
    pumping.store(false);
    gameThread.join();
    dispatcher.Stop();
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_StopCancelsPartialClientFrame)
{
    const auto state = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-partial-state") + ".txt");
    std::error_code error;
    std::filesystem::remove(state, error);
    const std::vector<uint8_t> key(32, 0x2a);
    const std::string endpoint = UniqueName("spark-area-control-partial-frame");
    LocalAreaControlService service(endpoint, key, state);
    EXPECT_TRUE(service.Start());

#ifdef _WIN32
    const std::wstring pipe = L"\\\\.\\pipe\\" + std::wstring(endpoint.begin(), endpoint.end());
    HANDLE client = CreateFileW(pipe.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    EXPECT_TRUE(client != INVALID_HANDLE_VALUE);
    if (client != INVALID_HANDLE_VALUE)
    {
        const uint8_t partial = 0x01;
        DWORD written = 0;
        EXPECT_TRUE(WriteFile(client, &partial, 1, &written, nullptr) != 0);
    }
#else
    const std::string socketPath = "/tmp/" + endpoint + ".sock";
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_TRUE(client >= 0);
    if (client >= 0)
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, socketPath.data(), socketPath.size());
        EXPECT_TRUE(::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
        const uint8_t partial = 0x01;
        EXPECT_TRUE(::send(client, &partial, 1, 0) == 1);
    }
#endif

    auto stopping = std::async(std::launch::async, [&service] { service.Stop(); });
    const bool stoppedPromptly = stopping.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
#ifdef _WIN32
    if (client != INVALID_HANDLE_VALUE)
        CloseHandle(client);
#else
    if (client >= 0)
        ::close(client);
#endif
    stopping.wait();
    EXPECT_TRUE(stoppedPromptly);
    std::filesystem::remove(state, error);
}

TEST(GatewayAreaControl_SecondServiceCannotStealLiveEndpoint)
{
    const std::string endpoint = UniqueName("spark-area-control-exclusive");
    const auto firstState = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-exclusive-a") + ".txt");
    const auto secondState =
        std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-exclusive-b") + ".txt");
    const std::vector<uint8_t> key(32, 0x7d);
    LocalAreaControlService first(endpoint, key, firstState);
    LocalAreaControlService second(endpoint, key, secondState);
    EXPECT_TRUE(first.Start());
    EXPECT_FALSE(second.Start());
    EXPECT_TRUE(second.GetLastError().find("area-control") != std::string::npos);
    EXPECT_TRUE(first.IsReady());
    first.Stop();
    std::error_code error;
    std::filesystem::remove(firstState, error);
    std::filesystem::remove(secondState, error);
}

TEST(GatewayApplication_IngressStartupFailureUnwindsAWorkingCoordinator)
{
    // Start() only reaches the ingress step after the WorldServer, the area
    // registrations and the authenticated area-control readiness probe already
    // succeeded, so this pins the one startup-failure path that has live state
    // to unwind. The ingress borrows a GatewayCoordinator reference and must not
    // survive it, which is only observable if the teardown order is preserved.
    const auto areaState = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-app-area") + ".txt");
    const auto squatterState =
        std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-app-squat") + ".txt");
    std::error_code error;
    std::filesystem::remove(areaState, error);
    std::filesystem::remove(squatterState, error);
    const std::vector<uint8_t> key(32, 0x3c);

    const uint16_t controlPort = UniquePort(41);
    const std::string ingressEndpoint = UniqueName("spark-gateway-app-ingress");

    LocalAreaControlService areaService("spark-area-control-" + std::to_string(controlPort), key, areaState);
    EXPECT_TRUE(areaService.Start());
    // Own the ingress endpoint first so the application's own listener cannot
    // be created on either a named pipe or a unix socket.
    LocalAreaControlService ingressSquatter(ingressEndpoint, key, squatterState);
    EXPECT_TRUE(ingressSquatter.Start());

    GatewayOptions options;
    options.world.worldName = "GatewayApplicationIngressFailure";
    options.world.port = UniquePort(42);
    options.world.interServerPort = UniquePort(43);
    options.ingressEndpoint = ingressEndpoint;
    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.areaName = "Loopback";
    area.area.port = UniquePort(44);
    area.area.interServerPort = controlPort;
    area.area.maxClients = 8;
    options.areas = {area};

    {
        GatewayApplication application(std::move(options), std::make_unique<KeyFileAuthenticator>(key),
                                       std::make_unique<LocalAreaControlPlane>(key));
        EXPECT_FALSE(application.Start());
        const GatewayHealth health = application.GetHealth();
        EXPECT_FALSE(health.live);
        EXPECT_FALSE(health.ready);
        EXPECT_FALSE(health.ingressReady);
        EXPECT_TRUE(health.lastError.find("ingress") != std::string::npos);
    }

    // The failed startup released the endpoint it could not claim, so the
    // owning service is still the only listener and is still serving.
    EXPECT_TRUE(ingressSquatter.IsReady());
    ingressSquatter.Stop();
    areaService.Stop();
    std::filesystem::remove(areaState, error);
    std::filesystem::remove(squatterState, error);
}

namespace
{
    // An adapter whose backend is down: it reports ready, then throws on every call.
    class ThrowingAuthenticator final : public IGatewayAuthenticator
    {
      public:
        AuthenticationResult Authenticate(const AdmissionRequest&) override
        {
            throw std::runtime_error("identity backend unreachable");
        }
        bool IsReady() const override { return true; }
    };
} // namespace

TEST(SparkGateway_GuardedAuthenticator_GatewayHealthReportsCircuit)
{
    // NET-110 observability: the guarded-authenticator counters and the open circuit reach the
    // running gateway's health JSON (the --health-file / status line), and readiness drops while
    // every admission is being failed fast.
    const auto areaState = std::filesystem::temp_directory_path() / (UniqueName("spark-gateway-health-area") + ".txt");
    std::error_code error;
    std::filesystem::remove(areaState, error);
    const std::vector<uint8_t> key(32, 0x5a);
    const uint16_t controlPort = UniquePort(51);
    const std::string ingressEndpoint = UniqueName("spark-gateway-health-ingress");

    LocalAreaControlService areaService("spark-area-control-" + std::to_string(controlPort), key, areaState);
    EXPECT_TRUE(areaService.Start());

    GatewayOptions options;
    options.world.worldName = "GatewayApplicationAuthenticationHealth";
    options.world.port = UniquePort(52);
    options.world.interServerPort = UniquePort(53);
    options.ingressEndpoint = ingressEndpoint;
    AreaEndpoint area;
    area.host = "127.0.0.1";
    area.area.areaName = "Loopback";
    area.area.port = UniquePort(54);
    area.area.interServerPort = controlPort;
    area.area.maxClients = 8;
    options.areas = {area};

    {
        GatewayApplication application(std::move(options), std::make_unique<ThrowingAuthenticator>(),
                                       std::make_unique<LocalAreaControlPlane>(key));
        ASSERT_TRUE(application.Start());
        EXPECT_TRUE(application.GetHealth().ready);
        EXPECT_TRUE(application.GetHealthJson().find("\"circuitOpen\":false") != std::string::npos);

        AdmissionRequest request;
        request.sessionId = "health-session";
        request.playerName = "HealthPlayer";
        request.credential = "opaque-health-credential";
        const LocalGatewayIngressClient client(ingressEndpoint);
        // Five consecutive adapter faults open the circuit (spec section 5.2); the sixth
        // admission is failed fast without reaching the adapter.
        for (Spark::Net::ClientID clientId = 1; clientId <= 6; ++clientId)
        {
            request.clientId = clientId;
            EXPECT_FALSE(client.Admit(request).accepted);
        }

        const GatewayHealth health = application.GetHealth();
        EXPECT_TRUE(health.live);
        EXPECT_FALSE(health.ready);
        EXPECT_TRUE(health.authenticationReady);
        EXPECT_EQ(health.authentication.faults, 5u);
        EXPECT_EQ(health.authentication.rejectedWhileOpen, 1u);
        EXPECT_TRUE(health.authentication.circuitOpen);

        const std::string json = application.GetHealthJson();
        EXPECT_TRUE(json.find("\"ready\":false") != std::string::npos);
        EXPECT_TRUE(json.find("\"authentication\":{\"accepted\":0,\"rejected\":0,\"faults\":5,\"budgetOverruns\":0,"
                              "\"rejectedWhileOpen\":1,\"consecutiveFaults\":5,\"circuitOpen\":true,"
                              "\"maxCallMicroseconds\":") != std::string::npos);
        EXPECT_TRUE(json.find(request.credential) == std::string::npos);
        application.Stop();
    }

    areaService.Stop();
    std::filesystem::remove(areaState, error);
}

TEST(GatewayAreaControl_EpochStateRejectsSignedNumbers)
{
    // SparkFuzzGatewayAreaControlState's regression seeds: std::istream numeric extraction
    // accepted a sign and negated it into the unsigned field, so "-1" loaded as the largest
    // epoch (no later Prepare could ever pass the fence) and "-4294967295" as phase 1.
    AreaControlSessions sessions;
    sessions["kept"] = AreaControlSessionFence{3, AreaControlPhase::Commit, 1, 2};
    const AreaControlSessions before = sessions;
    for (const std::string_view damaged : {
             "v2\n\"s\" -1 1 1 2\n",
             "v2\n\"s\" 1 -4294967295 1 2\n",
             "v2\n\"s\" +7 1 1 2\n",
             "v2\n\"s\" 7 1 -4294967295 2\n",
             "v2\n\"s\" 7 1 1 2x\n",
             "v2\n\"s\" 18446744073709551616 1 1 2\n",
         })
    {
        EXPECT_FALSE(ParseAreaControlState(damaged, sessions));
        EXPECT_EQ(sessions.size(), before.size());
        EXPECT_EQ(sessions.at("kept").epoch, uint64_t{3});
    }

    // What SaveState writes loads back unchanged, including quotes and spaces in an id.
    AreaControlSessions written;
    written["plain"] = AreaControlSessionFence{18446744073709551615ull, AreaControlPhase::Acknowledge, 7, 9};
    written["with \"quote\" and space"] = AreaControlSessionFence{1, AreaControlPhase::Prepare, 4294967295u, 1};
    AreaControlSessions reloaded;
    ASSERT_TRUE(ParseAreaControlState(SerializeAreaControlState(written), reloaded));
    ASSERT_EQ(reloaded.size(), written.size());
    for (const auto& [session, fence] : written)
    {
        ASSERT_TRUE(reloaded.contains(session));
        EXPECT_EQ(reloaded.at(session).epoch, fence.epoch);
        EXPECT_TRUE(reloaded.at(session).phase == fence.phase);
        EXPECT_EQ(reloaded.at(session).sourceArea, fence.sourceArea);
        EXPECT_EQ(reloaded.at(session).targetArea, fence.targetArea);
    }
    EXPECT_TRUE(ParseAreaControlState(" \n\t", reloaded));
    EXPECT_TRUE(reloaded.empty());
}
