// high_performance_tcp_lib.hpp
#ifndef HIGH_PERFORMANCE_TCP_LIB_HPP
#define HIGH_PERFORMANCE_TCP_LIB_HPP

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <map>

namespace hpnet {

// Packet types
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

// Status codes
enum class Status : uint8_t {
    Ok    = 0x00,
    Error = 0xFF
};

#pragma pack(push,1)
struct Header {
    uint32_t packet_length; // network order
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

using ErrorHandler    = std::function<void(const boost::system::error_code&)>;
using RequestHandler = std::function<void(const Packet&, ConnectionPtr)>;
using ResponseHandler = std::function<void(const Packet&)>;

struct Config {
    std::chrono::seconds reconnect_interval{5};
    std::chrono::seconds request_timeout{10};
    std::chrono::seconds watchdog_interval{30};
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
    explicit Connection(boost::asio::ip::tcp::socket socket);
    ~Connection();

    void start();
    void sendPacket(const Packet& pkt);
    void close();

    void onRequest(RequestHandler h);
    void onResponse(ResponseHandler h);
    void onError(ErrorHandler h);

    void sendResponse(const Packet& request, const Buffer& body, Status status = Status::Ok);

private:
    // I/O
    void doReadHeader();
    void doReadBody();
    void doWrite();

    // Buffer pool
    Buffer& acquireBuffer(std::size_t sz);
    std::vector<Buffer> bufferPool_;
    std::size_t poolIndex_{};

    // Ring write queue
    std::vector<Buffer> ring_;
    std::size_t head_{}, tail_{}, count_{};
    void enqueueWrite(Buffer buf);
    Buffer dequeueWrite();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::strand<boost::asio::any_io_executor> strand_;
    Header readHeader_;
    Buffer readBody_;

    RequestHandler requestHandler_;
    ResponseHandler responseHandler_;
    ErrorHandler errorHandler_;
};
} // namespace hpnet

#endif // HIGH_PERFORMANCE_TCP_LIB_HPP
