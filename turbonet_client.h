#pragma once

#include <asio.hpp>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <chrono>
#include <queue>

namespace turbonet {

using PacketHandler = std::function<void(uint8_t packetId,
                                         uint8_t status,
                                         uint32_t sequence,
                                         const std::vector<uint8_t>& payload)>;
using TimeoutHandler = std::function<void(uint32_t sequence)>;
using BindHandler = std::function<void(const std::string& serverId)>;

class TurboNetClient : public std::enable_shared_from_this<TurboNetClient> {
public:
    TurboNetClient(int readTimeoutMs = 0,
                   int writeTimeoutMs = 0,
                   int responseTimeoutMs = 0,
                   std::size_t ioThreads = std::thread::hardware_concurrency());
    ~TurboNetClient();

    // Connect to server with timeout
    void connect(const std::string& host,
                 uint16_t port,
                 int timeoutMs,
                 std::function<void(const asio::error_code&)> onConnect);

    // Set client-side ID to bind
    void setClientId(const std::string& clientId);
    // Bind-response handler
    void setBindHandler(BindHandler handler);

    // Send arbitrary packet
    void sendPacket(uint8_t packetId,
                    uint8_t status,
                    uint32_t sequence,
                    const uint8_t* data,
                    std::size_t len);

    // Send request packetId=0x02, status=0x00
    uint32_t sendRequest(const uint8_t* data,
                         std::size_t len);

    // Generic handlers
    void setPacketHandler(PacketHandler handler);
    void setTimeoutHandler(TimeoutHandler handler);

    // Close the connection
    void close();

private:
    // I/O methods
    void doReadHeader();
    void onReadHeader(const asio::error_code& ec, std::size_t bytes_transferred);
    void doReadBody(uint32_t bodyLen);
    void onReadBody(const asio::error_code& ec, std::size_t bytes_transferred);
    void doWrite();
    void onWrite(const asio::error_code& ec, std::size_t bytes_transferred);

    // Timers
    void startConnectTimer(int timeoutMs);
    void cancelConnectTimer();
    void onConnectTimeout(const asio::error_code& ec);

    void startReadTimer();
    void cancelReadTimer();
    void onReadTimeout(const asio::error_code& ec);

    void startWriteTimer();
    void cancelWriteTimer();
    void onWriteTimeout(const asio::error_code& ec);

    // Response timeout management
    void startResponseTimer(uint32_t sequence);
    void cancelResponseTimer(uint32_t sequence);
    void checkAndFireResponseTimers(const asio::error_code& ec);

    // Endian
    static uint32_t toBigEndian(uint32_t v);
    static uint32_t fromBigEndian(const uint8_t* b);

    asio::io_context                                  ioCtx_;
    asio::executor_work_guard<asio::io_context::executor_type> workGuard_;
    asio::ip::tcp::socket                             socket_;
    asio::steady_timer                                connectTimer_;
    asio::steady_timer                                readTimer_;
    asio::steady_timer                                writeTimer_;
    asio::steady_timer                                responseSweepTimer_;
    asio::strand<asio::io_context::executor_type>     strand_;
    std::vector<std::thread>                          ioThreads_;
    std::atomic<bool>                                 running_{false};

    std::array<uint8_t, 10>                           headerBuf_;
    std::vector<uint8_t>                              bodyBuf_;
    std::vector<uint8_t>                              txBuffer_;

    PacketHandler                                     onPacket_;
    TimeoutHandler                                    onTimeout_;
    BindHandler                                       onBind_;
    std::function<void(const asio::error_code&)>      connectHandler_;

    int                                               readTimeoutMs_;
    int                                               writeTimeoutMs_;
    int                                               responseTimeoutMs_;

    std::atomic<uint32_t>                             sequenceCounter_{1};

    struct ResponseEntry {
        uint32_t sequence;
        std::chrono::steady_clock::time_point expiry;
        bool operator>(const ResponseEntry& rhs) const { return expiry > rhs.expiry; }
    };
    std::priority_queue<ResponseEntry,
                            std::vector<ResponseEntry>,
                            std::greater<>>              responseQueue_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> responseMap_;
    std::mutex                                        responseMutex_;

    std::string                                       clientId_;
    uint8_t                                           lastPacketId_;
    uint8_t                                           lastStatus_;
    uint32_t                                          lastSequence_;
};

} // namespace turbonet