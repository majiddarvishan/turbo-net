// high_performance_tcp_lib.hpp
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

// Status byte
enum class Status : uint8_t {
    Ok    = 0x00,
    Error = 0xFF
};

#pragma pack(push, 1)
struct Header {
    uint32_t packet_length; // total length incl. header (network byte order)
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

class Connection;
using ConnectionPtr = std::shared_ptr<Connection>;

struct Config {
    std::chrono::seconds reconnect_interval{5};
    std::chrono::seconds request_timeout{10};
    std::chrono::seconds watchdog_interval{30};
};

// Base connection handling async I/O and fragmentation
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using ErrorHandler    = std::function<void(const boost::system::error_code&)>;
    using RequestHandler  = std::function<void(const Packet&, ConnectionPtr)>;
    using ResponseHandler = std::function<void(const Packet&)>;

    Connection(boost::asio::ip::tcp::socket socket);
    ~Connection();

    void start();
    void sendPacket(const Packet& pkt);
    void close();

    void onRequest(RequestHandler h);
    void onResponse(ResponseHandler h);
    void onError(ErrorHandler h);

protected:
    void doReadHeader();
    void doReadBody();
    void doWrite();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    Header readHeader_;
    Buffer readBody_;
    std::deque<Buffer> writeQueue_;

    RequestHandler requestHandler_;
    ResponseHandler responseHandler_;
    ErrorHandler errorHandler_;
};

// Server accepting connections
class Server {
public:
    using ConnectHandler = std::function<void(ConnectionPtr)>;

    Server(boost::asio::io_context& ioc, const boost::asio::ip::tcp::endpoint& ep);
    void startAccept();
    void onClientConnect(ConnectHandler h);

private:
    void doAccept();
    boost::asio::ip::tcp::acceptor acceptor_;
    ConnectHandler connectHandler_;
};

// Client with reconnect, timeout, watchdog
class Client : public std::enable_shared_from_this<Client> {
public:
    using ResponseCallback = std::function<void(const Packet&)>;
    using BindHandler      = std::function<void(const Packet&)>;
    using ErrorHandler     = std::function<void(const boost::system::error_code&)>;

    Client(boost::asio::io_context& ioc,
           const boost::asio::ip::tcp::endpoint& ep,
           Config cfg = {});
    void start();
    void stop();
    void sendRequest(const Packet& pkt, ResponseCallback cb);

    void onBindResp(BindHandler h);
    void onError(ErrorHandler h);

private:
    void doConnect();
    void scheduleReconnect();
    void scheduleTimeout(uint32_t seq);
    void scheduleWatchdog();

    void handlePacket(const Packet& pkt);
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

} // namespace hpnet

#endif // HIGH_PERFORMANCE_TCP_LIB_HPP
