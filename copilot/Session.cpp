#include "Session.h"
#include <iostream>
#include <cstring>
#include <algorithm>

using boost::asio::ip::tcp;

//
// Constructor: initializes the socket and the global timeout timer.
//
Session::Session(boost::asio::io_context& io_context)
    : socket_(io_context),
      timeout_timer_(std::make_shared<boost::asio::steady_timer>(io_context))
{
}

//
// Return the underlying socket.
//
boost::asio::ip::tcp::socket& Session::socket() {
    return socket_;
}

//
// Start the session by beginning an asynchronous header read.
//
void Session::start() {
    do_read_header();
}

//
// Queue data for writing. If nothing is in progress, start the asynchronous write.
//
void Session::write(const std::vector<char>& data) {
    bool write_in_progress = !write_msgs_.empty();
    write_msgs_.push_back(data);
    if (!write_in_progress) {
        do_write();
    }
}

//
// Asynchronously read the fixed-size header.
//
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
            std::cerr << "Error reading header: " << ec.message() << "\n";
        }
    });
}

//
// Asynchronously read the packet body.
//
void Session::do_read_body(const PacketHeader& header, std::size_t body_length) {
    auto self(shared_from_this());
    std::vector<char> body(body_length);
    boost::asio::async_read(socket_,
                            boost::asio::buffer(body),
                            [this, self, header, body](boost::system::error_code ec, std::size_t /*length*/) {
        if (!ec) {
            process_packet(header, body);
            do_read_header(); // Continue reading subsequent packets.
        } else {
            std::cerr << "Error reading body: " << ec.message() << "\n";
        }
    });
}

//
// Asynchronously write the front message in the write queue.
//
void Session::do_write() {
    auto self(shared_from_this());
    boost::asio::async_write(socket_,
                             boost::asio::buffer(write_msgs_.front()),
                             [this, self](boost::system::error_code ec, std::size_t /*length*/) {
        if (!ec) {
            write_msgs_.pop_front();
            if (!write_msgs_.empty()) {
                do_write();
            }
        } else {
            std::cerr << "Error writing message: " << ec.message() << "\n";
        }
    });
}

//
// Process a complete packet. If it's a response (packet_type == 0x02)
// and a pending request with a matching sequence exists, call its callback.
// Otherwise, forward it to on_packet_received if set.
//
void Session::process_packet(const PacketHeader& header, const std::vector<char>& body) {
    if (header.packet_type == 0x02) { // Response packet.
        auto it = pending_responses_.find(header.sequence);
        if (it != pending_responses_.end()) {
            // Remove the timer entry for this sequence.
            auto handle_it = pending_handles_.find(header.sequence);
            if (handle_it != pending_handles_.end()) {
                timer_heap_.erase(handle_it->second);
                pending_handles_.erase(handle_it);
            }
            // Call the on_response callback.
            auto callback = it->second;
            pending_responses_.erase(it);
            if (callback)
                callback(body);
            // Re-schedule the global timeout timer in case the earliest deadline changed.
            schedule_timeout_timer();
            return;
        }
    }
    // If not a response to a pending request, forward the packet.
    if (on_packet_received)
        on_packet_received(header, body);
}

//
// Send a request packet with the given payload and timeout. The method:
// 1. Builds the packet (header + body) with a unique sequence.
// 2. Stores the on_response callback.
// 3. Allocates a TimerEntry from the object pool, sets its deadline, and pushes it into the heap.
// 4. Sends the packet and re-schedules the global timeout timer.
//
void Session::send_request(const std::vector<char>& body,
                           int timeout_seconds,
                           std::function<void(const std::vector<char>&)> on_response,
                           std::function<void()> on_timeout)
{
    uint32_t sequence = next_sequence_++;
    PacketHeader header;
    header.packet_type = 0x01;  // Request packet.
    header.status = 0;
    header.sequence = sequence;
    header.packet_length = PacketHeader::header_length + static_cast<uint32_t>(body.size());

    // Construct the complete packet.
    std::vector<char> packet(header.packet_length);
    header.to_buffer(packet.data());
    if (!body.empty()) {
        std::memcpy(packet.data() + PacketHeader::header_length, body.data(), body.size());
    }

    // Save the on_response callback.
    pending_responses_[sequence] = on_response;

    // Allocate a TimerEntry for this request from the object pool.
    TimerEntry* entry = timer_pool_.construct();
    entry->sequence = sequence;
    entry->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    entry->on_timeout = on_timeout;

    // Insert the TimerEntry pointer into the heap and save its handle.
    auto handle = timer_heap_.push(entry);
    pending_handles_[sequence] = handle;

    // Send the packet.
    write(packet);

    // Re-schedule the global timeout timer.
    schedule_timeout_timer();
}

//
// Schedule the global timeout timer to fire when the earliest pending TimerEntry expires.
//
void Session::schedule_timeout_timer() {
    if (timer_heap_.empty()) {
        timeout_timer_->cancel();
        return;
    }
    // Get the earliest deadline from the heap.
    auto earliest_deadline = timer_heap_.top()->deadline;
    auto now = std::chrono::steady_clock::now();
    auto wait_duration = (earliest_deadline > now) ? earliest_deadline - now : std::chrono::milliseconds(0);

    timeout_timer_->expires_after(wait_duration);
    auto self(shared_from_this());
    timeout_timer_->async_wait([this, self](boost::system::error_code ec) {
        if (!ec)
            handle_timeout();
    });
}

//
// Called when the global timeout timer expires. It processes all TimerEntry objects
// whose deadlines have passed, calling their on_timeout callbacks and cleaning up bookkeeping.
// Then it re-schedules itself for the next pending deadline.
//
void Session::handle_timeout() {
    auto now = std::chrono::steady_clock::now();
    while (!timer_heap_.empty() && timer_heap_.top()->deadline <= now) {
        TimerEntry* expired = timer_heap_.top();
        timer_heap_.pop();
        pending_handles_.erase(expired->sequence);
        if (expired->on_timeout)
            expired->on_timeout();
        pending_responses_.erase(expired->sequence);
        // No need to explicitly delete the expired TimerEntry; the object_pool will reclaim memory when the pool is destroyed.
    }
    schedule_timeout_timer();
}
