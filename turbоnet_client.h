#pragma once

#include <asio.hpp>
#include <thread>
#include <atomic>
#include <functional>
#include <vector>
#include <memory>
#include <cstdint>
#include <chrono>

namespace turbоnet {

using DataHandler = std::function<void(const std::vector<uint8_t>&)>;

class TurboNetClient : public std::enable_shared_from_this<TurboNetClient> {
public:
    // readTimeoutMs and writeTimeoutMs in milliseconds, ioThreads = number of threads in pool
    TurboNetClient(int readTimeoutMs = 0, int writeTimeoutMs = 0, std::size_t ioThreads = std::thread::hardware_concurrency());
    ~TurboNetClient();

    // Connect with timeout in milliseconds
    void connect(const std::string& host,
                 uint16_t port,
                 int timeoutMs,
                 std::function<void(const asio::error_code&)> onConnect);

    // Enqueue a framed message: 4-byte length header (big-endian) + data
    void send(const uint8_t* data, std::size_t len);

    // Install callback to receive complete messages.
    void setDataHandler(DataHandler handler);

    // Close the connection.
    void close();

private:
    void doReadHeader();
    void onReadHeader(const asio::error_code& ec, std::size_t bytes_transferred);
    void doReadBody(std::uint32_t bodyLen);
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

    asio::io_context                                  ioCtx_;
    asio::ip::tcp::socket                             socket_;
    asio::steady_timer                                connectTimer_;
    asio::steady_timer                                readTimer_;
    asio::steady_timer                                writeTimer_;
    asio::strand<asio::io_context::executor_type>     strand_;
    std::vector<std::thread>                          ioThreads_;
    std::atomic<bool>                                 running_{false};

    // Framing: header buffer + dynamic body buffer
    std::array<uint8_t, 4>                            headerBuf_;
    std::vector<uint8_t>                              bodyBuf_;

    // Combined TX buffer: holds header+body sequences
    std::vector<uint8_t>                              txBuffer_;

    DataHandler                                       onData_;
    std::function<void(const asio::error_code&)>      connectHandler_;
    int                                               readTimeoutMs_;
    int                                               writeTimeoutMs_;
};

} // namespace turbоnet

