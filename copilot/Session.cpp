#include "Session.h"
#include <iostream>
#include <cstring>
#include <algorithm>
using boost::asio::ip::tcp;

Session::Session(boost::asio::io_context& io_context)
    : socket_(io_context)
    , timeout_timer_(std::make_shared<boost::asio::steady_timer>(io_context))
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
                process_packet(header, body);
                do_read_header();  // Continue reading next packets.
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

void Session::process_packet(const PacketHeader& header, const std::vector<char>& body) {
    // Check if this packet is a response (packet type 0x02)
    if (header.packet_type == 0x02) {
        auto it = pending_requests_.find(header.sequence);
        if (it != pending_requests_.end()) {
            // Call the on_response callback.
            auto callback = it->second.on_response;
            pending_requests_.erase(it);
            if (callback)
                callback(body);
            // Reschedule the timer as the earliest deadline may have changed.
            schedule_timeout_timer();
            return;
        }
    }
    // If not a response to a pending request, forward to the generic handler.
    if (on_packet_received)
        on_packet_received(header, body);
}

void Session::send_request(const std::vector<char>& body,
                           int timeout_seconds,
                           std::function<void(const std::vector<char>&)> on_response,
                           std::function<void()> on_timeout)
{
    uint32_t sequence = next_sequence_++;
    PacketHeader header;
    header.packet_type = 0x01;  // 0x01 means "request".
    header.status = 0;
    header.sequence = sequence;
    header.packet_length = PacketHeader::header_length + static_cast<uint32_t>(body.size());

    // Build the full packet.
    std::vector<char> packet(header.packet_length);
    header.to_buffer(packet.data());
    if (!body.empty())
        std::memcpy(packet.data() + PacketHeader::header_length, body.data(), body.size());

    // Create a pending request entry with an expiration deadline.
    PendingRequest req;
    req.on_response = on_response;
    req.on_timeout = on_timeout;
    req.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    pending_requests_[sequence] = req;

    // Send the packet.
    write(packet);

    // Update the timeout timer to fire at the earliest deadline.
    schedule_timeout_timer();
}

void Session::schedule_timeout_timer() {
    if (pending_requests_.empty()) {
        timeout_timer_->cancel();
        return;
    }

    // Find the earliest deadline among pending requests.
    auto now = std::chrono::steady_clock::now();
    auto earliest = std::min_element(
        pending_requests_.begin(), pending_requests_.end(),
        [](const auto &a, const auto &b) {
            return a.second.deadline < b.second.deadline;
        });

    // Determine the duration until that deadline.
    auto wait_duration = earliest->second.deadline - now;
    timeout_timer_->expires_after(wait_duration);

    auto self(shared_from_this());
    timeout_timer_->async_wait([this, self](boost::system::error_code ec) {
        if (ec)
            return; // likely cancelled

        auto now = std::chrono::steady_clock::now();
        std::vector<uint32_t> expired;
        for (auto& [seq, req] : pending_requests_) {
            if (req.deadline <= now) {
                if (req.on_timeout)
                    req.on_timeout();
                expired.push_back(seq);
            }
        }
        // Remove expired requests.
        for (auto seq : expired)
            pending_requests_.erase(seq);

        // Reschedule the timer for the next pending request, if any.
        if (!pending_requests_.empty()) {
            schedule_timeout_timer();
        }
    });
}
