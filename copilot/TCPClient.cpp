#include "TCPClient.h"
#include "PacketHeader.h"
#include <iostream>
#include <chrono>
using boost::asio::ip::tcp;

TCPClient::TCPClient(boost::asio::io_context& io_context,
                     const boost::asio::ip::tcp::resolver::results_type& endpoints)
    : io_context_(io_context),
      socket_(io_context),
      endpoints_(endpoints),
      reconnect_timer_(io_context)
{
    // Do not call start_connect() here. Instead, call start() after constructing a shared_ptr.
}

void TCPClient::start() {
    start_connect();
}

void TCPClient::start_connect() {
    auto self = shared_from_this();
    boost::asio::async_connect(socket_, endpoints_,
        [this, self](boost::system::error_code ec, tcp::endpoint) {
            if (!ec) {
                // Create a new Session (which uses a BufferPool and TimerEntry object pool).
                session_ = std::make_shared<Session>(io_context_);
                session_->socket() = std::move(socket_);
                session_->start();
                set_session(session_);
            } else {
                std::cerr << "Connection error: " << ec.message() << "\n";
                schedule_reconnect();
            }
        }
    );
}

void TCPClient::schedule_reconnect() {
    reconnect_timer_.expires_after(std::chrono::seconds(3));
    auto self = shared_from_this();
    reconnect_timer_.async_wait([this, self](boost::system::error_code ec) {
        if (!ec)
            start_connect();
    });
}

void TCPClient::set_session(std::shared_ptr<Session> session) {
    session_ = session;
    // As an example, set a generic handler for packets that are not responses.
    session_->on_packet_received = [this](const PacketHeader& header, std::string_view body_view) {
        std::cout << "Client received unhandled packet, seq: "
                  << header.sequence << " body: " << std::string(body_view) << "\n";
    };
}

void TCPClient::send_request(const std::vector<char>& body,
                             int timeout_seconds,
                             std::function<void(std::string_view)> on_response,
                             std::function<void()> on_timeout)
{
    if (session_) {
        session_->send_request(body, timeout_seconds, on_response, on_timeout);
    } else {
        std::cerr << "No active session available. Unable to send request!\n";
    }
}
