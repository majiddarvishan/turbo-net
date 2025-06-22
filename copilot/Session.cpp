#include "Session.h"
#include <iostream>
#include <algorithm>

using boost::asio::ip::tcp;

Session::Session(boost::asio::io_context& io_context)
    : socket_(io_context),
      timeout_timer_(std::make_shared<boost::asio::steady_timer>(io_context)),
      // Here we pre‑allocate buffers of 4096 bytes; adjust parameters as needed.
      readBufferPool_(4096, 128)
{
}

boost::asio::ip::tcp::socket& Session::socket() {
    return socket_;
}

void Session::start() {
    do_read_header();
}

void Session::write(const std::vector<char>& data) {
    bool in_progress = !write_msgs_.empty();
    write_msgs_.push_back(data);
    if (!in_progress)
        do_write();
}

void Session::do_read_header() {
    auto self(shared_from_this());
    // Acquire a buffer from the pool for the fixed‐size header.
    char* header_buf = readBufferPool_.acquire();
    boost::asio::async_read(socket_,
        boost::asio::buffer(header_buf, PacketHeader::header_length),
        [this, self, header_buf](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                PacketHeader header;
                header.from_buffer(header_buf);
                readBufferPool_.release(header_buf);
                std::size_t body_length = header.packet_length - PacketHeader::header_length;
                do_read_body(header, body_length);
            } else {
                std::cerr << "Error reading header: " << ec.message() << "\n";
                readBufferPool_.release(header_buf);
            }
        }
    );
}

void Session::do_read_body(const PacketHeader& header, std::size_t body_length) {
    auto self(shared_from_this());
    // Acquire a buffer for the body.
    // If body_length is less than or equal to our pool’s buffer size, use the pool.
    char* body_buf = nullptr;
    if (body_length <= readBufferPool_.buffer_size()) {
        body_buf = readBufferPool_.acquire();
    } else {
        // If the body is larger than our pre‑allocated buffer size, allocate dynamically.
        body_buf = new char[body_length];
    }
    boost::asio::async_read(socket_,
        boost::asio::buffer(body_buf, body_length),
        [this, self, header, body_buf, body_length](boost::system::error_code ec, std::size_t /*length*/) {
            if (!ec) {
                // Construct a string_view over the received data without copying.
                std::string_view body_view(body_buf, body_length);
                process_packet(header, body_view);
                // Release the buffer back to the pool or free it.
                if (body_length <= readBufferPool_.buffer_size()) {
                    readBufferPool_.release(body_buf);
                } else {
                    delete[] body_buf;
                }
                do_read_header();
            } else {
                std::cerr << "Error reading body: " << ec.message() << "\n";
                if (body_length <= readBufferPool_.buffer_size()) {
                    readBufferPool_.release(body_buf);
                } else {
                    delete[] body_buf;
                }
            }
        }
    );
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
                std::cerr << "Error writing message: " << ec.message() << "\n";
            }
        }
    );
}

void Session::process_packet(const PacketHeader& header, std::string_view body_view) {
    // If the packet is a response (packet_type == 0x02) and a matching request is pending…
    if (header.packet_type == 0x02) {
        auto it = pending_responses_.find(header.sequence);
        if (it != pending_responses_.end()) {
            auto callback = it->second;
            pending_responses_.erase(it);
            // Also remove from the timer heap if present.
            auto handle_it = pending_handles_.find(header.sequence);
            if (handle_it != pending_handles_.end()) {
                timer_heap_.erase(handle_it->second);
                pending_handles_.erase(handle_it);
            }
            if (callback)
                callback(body_view);
            schedule_timeout_timer();
            return;
        }
    }
    // Otherwise, forward the packet using the generic callback.
    if (on_packet_received)
        on_packet_received(header, body_view);
}

void Session::send_request(const std::vector<char>& body,
                           int timeout_seconds,
                           std::function<void(std::string_view)> on_response,
                           std::function<void()> on_timeout)
{
    uint32_t sequence = next_sequence_++;
    PacketHeader header;
    header.packet_type = 0x01;  // Request packet.
    header.status = 0;
    header.sequence = sequence;
    header.packet_length = PacketHeader::header_length + static_cast<uint32_t>(body.size());

    // Build the full packet.
    std::vector<char> packet(header.packet_length);
    header.to_buffer(packet.data());
    if (!body.empty()) {
        std::memcpy(packet.data() + PacketHeader::header_length,
                    body.data(),
                    body.size());
    }

    // Store the on_response callback.
    pending_responses_[sequence] = on_response;

    // Allocate a TimerEntry from the object pool.
    TimerEntry* entry = timer_pool_.construct();
    entry->sequence = sequence;
    entry->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    entry->on_timeout = on_timeout;

    // Insert the TimerEntry pointer into the heap.
    auto handle = timer_heap_.push(entry);
    pending_handles_[sequence] = handle;

    write(packet);
    schedule_timeout_timer();
}

void Session::schedule_timeout_timer() {
    if (timer_heap_.empty()) {
        timeout_timer_->cancel();
        return;
    }
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

void Session::handle_timeout() {
    auto now = std::chrono::steady_clock::now();
    while (!timer_heap_.empty() && timer_heap_.top()->deadline <= now) {
        TimerEntry* expired = timer_heap_.top();
        timer_heap_.pop();
        pending_handles_.erase(expired->sequence);
        if (expired->on_timeout)
            expired->on_timeout();
        pending_responses_.erase(expired->sequence);
        // No explicit delete is required—the object_pool will reclaim the memory on destruction.
    }
    schedule_timeout_timer();
}
