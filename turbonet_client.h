// File: turbonet_client.h
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

class TurboNetClient : public std::enable_shared_from_this<TurboNetClient> {
public:
    TurboNetClient(int readTimeoutMs = 0,
                   int writeTimeoutMs = 0,
                   int responseTimeoutMs = 0,
                   std::size_t ioThreads = std::thread::hardware_concurrency());
    ~TurboNetClient();

    void connect(const std::string& host,
                 uint16_t port,
                 int timeoutMs,
                 std::function<void(const asio::error_code&)> onConnect);

    void sendPacket(uint8_t packetId,
                    uint8_t status,
                    uint32_t sequence,
                    const uint8_t* data,
                    std::size_t len);

    uint32_t sendRequest(const uint8_t* data,
                         std::size_t len);

    void setPacketHandler(PacketHandler handler);
    void setTimeoutHandler(TimeoutHandler handler);

    void close();

private:
    // I/O and timers
    void doReadHeader();
    void onReadHeader(const asio::error_code& ec, std::size_t bytes_transferred);
    void doReadBody(uint32_t bodyLen);
    void onReadBody(const asio::error_code& ec, std::size_t bytes_transferred);
    void doWrite();
    void onWrite(const asio::error_code& ec, std::size_t bytes_transferred);

    void startConnectTimer(int timeoutMs);
    void cancelConnectTimer();
    void onConnectTimeout(const asio::error_code& ec);

    void startReadTimer();
    void cancelReadTimer();
    void onReadTimeout(const asio::error_code& ec);

    void startWriteTimer();
    void cancelWriteTimer();
    void onWriteTimeout(const asio::error_code& ec);

    // Response timeouts using single timer + min-heap
    void startResponseTimer(uint32_t sequence);
    void cancelResponseTimer(uint32_t sequence);
    void checkAndFireResponseTimers(const asio::error_code& ec);

    // Helpers
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
                            std::greater<>>                 responseQueue_;
    std::unordered_map<uint32_t, std::chrono::steady_clock::time_point> responseMap_;
    std::mutex                                        responseMutex_;

    uint8_t                                           lastPacketId_;
    uint8_t                                           lastStatus_;
    uint32_t                                          lastSequence_;
};

} // namespace turbonet
