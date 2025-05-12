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

namespace turbonet {

using PacketHandler = std::function<void(uint8_t packetId, uint8_t status, uint32_t sequence, const std::vector<uint8_t>& payload)>;
using TimeoutHandler = std::function<void(uint32_t sequence)>;

class TurboNetClient : public std::enable_shared_from_this<TurboNetClient> {
public:
    // readTimeoutMs and writeTimeoutMs in milliseconds, ioThreads = number of threads in pool
    TurboNetClient(int readTimeoutMs = 0, int writeTimeoutMs = 0, int responseTimeoutMs = 0,
                   std::size_t ioThreads = std::thread::hardware_concurrency());
    ~TurboNetClient();

    // Connect with timeout in milliseconds
    void connect(const std::string& host,
                 uint16_t port,
                 int timeoutMs,
                 std::function<void(const asio::error_code&)> onConnect);

    // Enqueue a framed packet: header (10 bytes) + payload
    // header: [4 bytes total length][1 byte packetId][1 byte status][4 bytes sequence]
    void sendPacket(uint8_t packetId, uint8_t status, uint32_t sequence, const uint8_t* data, std::size_t len);

    // Shortcut for request packets: packetId=0x02, status=0x00, auto sequence
    // Returns the sequence number used
    uint32_t sendRequest(const uint8_t* data, std::size_t len);

    // Install callback to receive complete packets.
    void setPacketHandler(PacketHandler handler);
    // Install callback for request timeout
    void setTimeoutHandler(TimeoutHandler handler);

    // Close the connection.
    void close();

private:
    // Read/write handlers
    void doReadHeader();
    void onReadHeader(const asio::error_code& ec, std::size_t bytes_transferred);
    void doReadBody(uint32_t bodyLen);
    void onReadBody(const asio::error_code& ec, std::size_t bytes_transferred);
    void doWrite();
    void onWrite(const asio::error_code& ec, std::size_t bytes_transferred);

    // Connect timer
    void startConnectTimer(int timeoutMs);
    void cancelConnectTimer();
    void onConnectTimeout(const asio::error_code& ec);
    // Read timer
    void startReadTimer();
    void cancelReadTimer();
    void onReadTimeout(const asio::error_code& ec);
    // Write timer
    void startWriteTimer();
    void cancelWriteTimer();
    void onWriteTimeout(const asio::error_code& ec);

    // Response timeout
    void startResponseTimer(uint32_t sequence);
    void cancelResponseTimer(uint32_t sequence);
    void onResponseTimeout(uint32_t sequence, const asio::error_code& ec);

    asio::io_context                                  ioCtx_;
    asio::ip::tcp::socket                             socket_;
    asio::steady_timer                                connectTimer_;
    asio::steady_timer                                readTimer_;
    asio::steady_timer                                writeTimer_;
    asio::strand<asio::io_context::executor_type>     strand_;
    std::vector<std::thread>                          ioThreads_;
    std::atomic<bool>                                 running_{false};

    // Framing: header (10 bytes) and dynamic body buffer
    std::array<uint8_t, 10>                           headerBuf_;
    std::vector<uint8_t>                              bodyBuf_;

    // Combined TX buffer: holds header+payload sequences
    std::vector<uint8_t>                              txBuffer_;

    PacketHandler                                     onPacket_;
    TimeoutHandler                                    onTimeout_;
    std::function<void(const asio::error_code&)>      connectHandler_;
    int                                               readTimeoutMs_;
    int                                               writeTimeoutMs_;
    int                                               responseTimeoutMs_;

    // Sequence generator for requests
    std::atomic<uint32_t>                             sequenceCounter_{1};

    // Response timers
    std::unordered_map<uint32_t, std::unique_ptr<asio::steady_timer>> responseTimers_;
    std::mutex                                        responseMutex_;

    // Temporary storage for last header fields
    uint8_t                                           lastPacketId_;
    uint8_t                                           lastStatus_;
    uint32_t                                          lastSequence_;
};

} // namespace turbonet