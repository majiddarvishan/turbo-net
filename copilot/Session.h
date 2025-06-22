#ifndef SESSION_H
#define SESSION_H

#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <boost/asio.hpp>
#include "PacketHeader.h"

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(boost::asio::io_context& io_context);
    boost::asio::ip::tcp::socket& socket();
    void start();
    void write(const std::vector<char>& data);

    // Callback invoked when a complete packet (header + body) is received.
    std::function<void(const PacketHeader&, const std::vector<char>&)> on_packet_received;

private:
    void do_read_header();
    void do_read_body(const PacketHeader& header, std::size_t body_length);
    void do_write();

    boost::asio::ip::tcp::socket socket_;
    std::deque<std::vector<char>> write_msgs_;
    std::vector<char> read_buffer_;
};

#endif // SESSION_H
