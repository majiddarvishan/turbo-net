#ifndef HIGH_PERFORMANCE_TCP_LIB_HPP
#define HIGH_PERFORMANCE_TCP_LIB_HPP

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <iostream>

namespace hpnet {

// Packet type definitions
enum class PacketType : uint8_t {
    BindReq     = 0x01,
    BindResp    = 0x81,
    StreamReq   = 0x02,
    StreamResp  = 0x82,
    UnbindReq   = 0x03,
    UnbindResp  = 0x83,
    WatchdogReq = 0x04,
    WatchdogResp= 0x84
};

// Status byte (customizable by user)
enum class Status : uint8_t {
    Ok       = 0x00,
    Error    = 0xFF
};

// Fixed-size header
#pragma pack(push, 1)
struct Header {
    uint32_t packet_length; // total packet length including header
    PacketType type;
    Status status;
    uint32_t sequence;
};
#pragma pack(pop)

typedef std::vector<uint8_t> Buffer;

struct Packet {
    Header header;
    Buffer body;
};

// Forward declarations
class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;

// User configuration
struct Config {
    std::chrono::seconds reconnect_interval{5};
    std::chrono::seconds request_timeout{10};
    std::chrono::seconds watchdog_interval{30};
};

// Base connection handles async I/O and fragmentation
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using ErrorHandler = std::function<void(const boost::system::error_code&)>;
    using RequestHandler = std::function<void(const Packet&, ConnectionPtr)>;
    using ResponseHandler = std::function<void(const Packet&)>;

    Connection(boost::asio::ip::tcp::socket socket)
        : socket_(std::move(socket)), strand_(socket_.get_executor()) {}

    virtual ~Connection() { close(); }

    void start();
    void sendPacket(const Packet& pkt);
    void close();

    void onRequest(RequestHandler handler) { requestHandler_ = std::move(handler); }
    void onResponse(ResponseHandler handler) { responseHandler_ = std::move(handler); }
    void onError(ErrorHandler handler) { errorHandler_ = std::move(handler); }

protected:
    void doReadHeader();
    void doReadBody();
    void doWrite();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    Header readHeader_;
    Buffer readBody_;
    std::deque<Buffer> writeQueue_;

    RequestHandler requestHandler_;
    ResponseHandler responseHandler_;
    ErrorHandler errorHandler_;
};

// Server implementation
class Server {
public:
    using ConnectHandler = std::function<void(ConnectionPtr)>;

    Server(boost::asio::io_context& io_context,
           const boost::asio::ip::tcp::endpoint& endpoint)
        : acceptor_(io_context, endpoint) {}

    void startAccept() { doAccept(); }
    void onClientConnect(ConnectHandler handler) { connectHandler_ = std::move(handler); }

private:
    void doAccept();

    boost::asio::ip::tcp::acceptor acceptor_;
    ConnectHandler connectHandler_;
};

// Client implementation
class Client : public std::enable_shared_from_this<Client> {
public:
    using ResponseCallback = std::function<void(const Packet&)>;
    using BindHandler      = std::function<void(const Packet&)>;
    using ErrorHandler     = std::function<void(const boost::system::error_code&)>;

    Client(boost::asio::io_context& io_context,
           const boost::asio::ip::tcp::endpoint& endpoint,
           Config config = {})
        : io_context_(io_context)
        , endpoint_(endpoint)
        , socket_(io_context)
        , reconnectTimer_(io_context)
        , watchdogTimer_(io_context)
        , config_(config)
        , nextSequence_(1) {}

    void start();
    void stop();
    void sendRequest(Packet pkt, ResponseCallback cb);

    void onBindResp(BindHandler handler) { bindHandler_ = std::move(handler); }
    void onError(ErrorHandler handler) { errorHandler_ = std::move(handler); }

private:
    void doConnect();
    void scheduleReconnect();
    void scheduleTimeout(uint32_t seq);
    void scheduleWatchdog();

    void handleRead(const Packet& pkt);
    void handleWrite();
    void handleTimeout(uint32_t seq);
    void sendBind();
    void sendWatchdog();

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::endpoint endpoint_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::steady_timer reconnectTimer_;
    boost::asio::steady_timer watchdogTimer_;
    Config config_;

    std::atomic<uint32_t> nextSequence_;
    std::unordered_map<uint32_t, ResponseCallback> pendingRequests_;
    BindHandler bindHandler_;
    ErrorHandler errorHandler_;
};

}

#endif