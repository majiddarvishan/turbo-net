#include "Session.h"
#include <iostream>

using boost::asio::ip::tcp;

Session::Session(boost::asio::io_context& io_context)
    : socket_(io_context)
{
}

tcp::socket& Session::socket() {
    return socket_;
}

void Session::start() {
    do_read_header();
}

void Session::write(const std::vector<char>& data) {
    bool write_in_progress = !write_msgs_.empty();
    write_msgs_.push_back(data);
    if (!write_in_progress)
        do_write();
}

void Session::do_read_header() {
    auto self(shared_from_this());
    read_buffer_.resize(PacketHeader::header_length);
    boost::asio::async_read(socket_,
        boost::asio::buffer(read_buffer_),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                PacketHeader header;
                header.from_buffer(read_buffer_.data());
                std::size_t body_length = header.packet_length - PacketHeader::header_length;
                do_read_body(header, body_length);
            } else {
                std::cerr << "Error reading header: " << ec.message() << std::endl;
            }
        });
}

void Session::do_read_body(const PacketHeader& header, std::size_t body_length) {
    auto self(shared_from_this());
    std::vector<char> body(body_length);
    boost::asio::async_read(socket_,
        boost::asio::buffer(body),
        [this, self, header, body](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                if (on_packet_received)
                    on_packet_received(header, body);
                do_read_header(); // Continue reading subsequent packets.
            } else {
                std::cerr << "Error reading body: " << ec.message() << std::endl;
            }
        });
}

void Session::do_write() {
    auto self(shared_from_this());
    boost::asio::async_write(socket_,
        boost::asio::buffer(write_msgs_.front()),
        [this, self](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                write_msgs_.pop_front();
                if (!write_msgs_.empty())
                    do_write();
            } else {
                std::cerr << "Error writing message: " << ec.message() << std::endl;
            }
        });
}
