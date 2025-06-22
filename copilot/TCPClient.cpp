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
    // Do not call start_connect() here.
    // It will be invoked in the start() method once the object is owned by a shared_ptr.
}

void TCPClient::start() {
    start_connect();
}

void TCPClient::send_request(const std::vector<char>& body,
                             uint8_t packet_type,
                             uint8_t status,
                             int timeout_seconds,
                             std::function<void(const std::vector<char>&)> on_response,
                             std::function<void()> on_timeout)
{
    uint32_t sequence = next_sequence_++;
    PacketHeader header;
    header.packet_type = packet_type;
    header.status = status;
    header.sequence = sequence;
    header.packet_length = PacketHeader::header_length + static_cast<uint32_t>(body.size());

    // Prepare full packet.
    std::vector<char> packet(header.packet_length);
    header.to_buffer(packet.data());
    if (!body.empty())
        std::memcpy(packet.data() + PacketHeader::header_length, body.data(), body.size());

    // Create and start a timer for this request's timeout.
    auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
    pending_timers_[sequence] = timer;
    timer->expires_after(std::chrono::seconds(timeout_seconds));
    timer->async_wait([this, sequence](boost::system::error_code ec) {
        if (!ec) {
            auto it = pending_requests_.find(sequence);
            if (it != pending_requests_.end()) {
                if (it->second.on_timeout) {
                    // Call the user-specified timeout function.
                    it->second.on_timeout();
                } else {
                    std::cerr << "Request timed out for sequence: " << sequence << std::endl;
                }
                pending_requests_.erase(it);
            }
        }
    });

    // Store callbacks for response and timeout.
    PendingRequest req;
    req.on_response = on_response ? on_response :
        [sequence](const std::vector<char>&) { std::cout << "Received response for sequence " << sequence << std::endl; };
    req.on_timeout = on_timeout;
    pending_requests_[sequence] = req;

    // Write the packet if the session is ready.
    if (session_)
        session_->write(packet);
}

void TCPClient::start_connect() {
    auto self = shared_from_this();  // Safe here because start() is called after construction.
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
        // Process responses: expecting packet_type 0x02.
        if (header.packet_type == 0x02) {
            auto it = pending_requests_.find(header.sequence);
            if (it != pending_requests_.end()) {
                // Cancel the timeout timer.
                auto timerIt = pending_timers_.find(header.sequence);
                if (timerIt != pending_timers_.end()) {
                    timerIt->second->cancel();
                    pending_timers_.erase(timerIt);
                }
                // Invoke the response callback.
                auto callback = it->second.on_response;
                pending_requests_.erase(it);
                callback(body);
            }
        }
    };
}
