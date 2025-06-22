#include "TCPClient.h"
#include "PacketHeader.h"
#include <iostream>
#include <chrono>
#include <cstring>
using boost::asio::ip::tcp;

TCPClient::TCPClient(boost::asio::io_context& io_context,
                     const boost::asio::ip::tcp::resolver::results_type& endpoints)
    : io_context_(io_context),
      socket_(io_context),
      endpoints_(endpoints),
      reconnect_timer_(io_context)
{
    start_connect();
}

void TCPClient::send_request(const std::vector<char>& body,
                             uint8_t packet_type,
                             uint8_t status,
                             int timeout_seconds)
{
    uint32_t sequence = next_sequence_++;
    PacketHeader header;
    header.packet_type = packet_type;
    header.status = status;
    header.sequence = sequence;
    header.packet_length = PacketHeader::header_length + static_cast<uint32_t>(body.size());

    std::vector<char> packet(header.packet_length);
    header.to_buffer(packet.data());
    if (!body.empty())
        std::memcpy(packet.data() + PacketHeader::header_length, body.data(), body.size());

    // Set up a timer for this request's timeout.
    auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
    timer->expires_after(std::chrono::seconds(timeout_seconds));
    timer->async_wait([this, sequence](boost::system::error_code ec) {
        if (!ec) {
            auto it = pending_responses_.find(sequence);
            if (it != pending_responses_.end()) {
                std::cerr << "Request timed out for sequence: " << sequence << std::endl;
                pending_responses_.erase(it);
            }
        }
    });
    pending_timers_[sequence] = timer;

    // Save a callback to process the response when it arrives.
    pending_responses_[sequence] = [sequence](const std::vector<char>& response_body) {
        std::cout << "Received response for sequence " << sequence << std::endl;
        // Process response_body as needed.
    };

    if (session_)
        session_->write(packet);
}

void TCPClient::start_connect() {
    auto self = shared_from_this();
    boost::asio::async_connect(socket_, endpoints_,
        [this, self](boost::system::error_code ec, tcp::endpoint) {
            if (!ec) {
                session_ = std::make_shared<Session>(io_context_);
                session_->socket() = std::move(socket_);
                session_->start();
                set_session(session_);
            } else {
                schedule_reconnect();
            }
        });
}

void TCPClient::schedule_reconnect() {
    reconnect_timer_.expires_after(std::chrono::seconds(3));
    reconnect_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec)
            start_connect();
    });
}

void TCPClient::set_session(std::shared_ptr<Session> session) {
    session_ = session;
    session_->on_packet_received = [this](const PacketHeader& header, const std::vector<char>& body) {
        // Process responses (packet_type 0x02).
        if (header.packet_type == 0x02) {
            auto it = pending_responses_.find(header.sequence);
            if (it != pending_responses_.end()) {
                pending_timers_[header.sequence]->cancel();
                pending_timers_.erase(header.sequence);
                auto callback = it->second;
                pending_responses_.erase(it);
                callback(body);
            }
        }
        // Additional handling for incoming requests can be added here.
    };
}
