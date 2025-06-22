#ifndef SESSION_H
#define SESSION_H

#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <map>
#include <boost/asio.hpp>
#include "PacketHeader.h"

// Structure to hold callbacks for a pending request.
struct PendingRequest {
    std::function<void(const std::vector<char>&)> on_response;
    std::function<void()> on_timeout;
};

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(boost::asio::io_context& io_context);
    boost::asio::ip::tcp::socket& socket();
    void start();
    void write(const std::vector<char>& data);

    // Generic callback for packets that are not responses to a sent request.
    // For example, this can be used to process inbound requests.
    std::function<void(const PacketHeader&, const std::vector<char>&)> on_packet_received;

    // New method: send a request (packet type 0x01) on this session.
    // The session waits for a response (packet type 0x02) with a matching sequence.
    // If no response is received within timeout_seconds, the on_timeout callback is invoked.
    void send_request(const std::vector<char>& body,
                      int timeout_seconds,
                      std::function<void(const std::vector<char>&)> on_response,
                      std::function<void()> on_timeout);

private:
    void do_read_header();
    void do_read_body(const PacketHeader& header, std::size_t body_length);
    void do_write();
    // Process a complete packet: route responses to pending requests; otherwise forward.
    void process_packet(const PacketHeader& header, const std::vector<char>& body);

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::socket socket_;

    std::deque<std::vector<char>> write_msgs_;
    std::vector<char> read_buffer_;

    // For outbound request handling.
    uint32_t next_sequence_ { 1 };
    std::map<uint32_t, PendingRequest> pending_requests_;
    std::map<uint32_t, std::shared_ptr<boost::asio::steady_timer>> pending_timers_;
};

#endif // SESSION_H
