#ifndef SESSION_H
#define SESSION_H

#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <unordered_map>
#include <string_view>
#include <cstring>
#include <boost/asio.hpp>
#include <boost/heap/fibonacci_heap.hpp>
#include <boost/pool/object_pool.hpp>

#include "PacketHeader.h"  // Must define PacketHeader and PacketHeader::header_length
#include "BufferPool.h"    // A simple buffer pool for zero‑copy read buffers

// Structure representing a timer entry (for pending timeouts)
struct TimerEntry {
    uint32_t sequence;
    std::chrono::steady_clock::time_point deadline;
    std::function<void()> on_timeout;
};

// Comparator for TimerEntry pointers (the earlier the deadline, the higher the priority)
struct TimerEntryPtrCompare {
    bool operator()(const TimerEntry* a, const TimerEntry* b) const {
        return a->deadline > b->deadline;
    }
};

// Define a Fibonacci heap (min‑heap) containing TimerEntry pointers.
using TimerHeap = boost::heap::fibonacci_heap<TimerEntry*, boost::heap::compare<TimerEntryPtrCompare>>;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(boost::asio::io_context& io_context);

    // Get the underlying socket.
    boost::asio::ip::tcp::socket& socket();

    // Start the session (begins asynchronous read operations).
    void start();

    // Queue up data for writing.
    // (We leave the write path unchanged; these buffers are typically produced by the user.)
    void write(const std::vector<char>& data);

    // Callback (for packets that are not responses for send_request).
    // Uses a zero‑copy std::string_view over the received body.
    std::function<void(const PacketHeader&, std::string_view)> on_packet_received;

    // Send a request with the specified payload.
    // The response callback receives a std::string_view (zero‑copy view) of the response data.
    // on_timeout is invoked if no response arrives within timeout_seconds.
    void send_request(const std::vector<char>& body,
                      int timeout_seconds,
                      std::function<void(std::string_view)> on_response,
                      std::function<void()> on_timeout);

private:
    // Asynchronous read routines.
    void do_read_header();
    void do_read_body(const PacketHeader& header, std::size_t body_length);

    // Asynchronous write routine.
    void do_write();

    // Process a received complete packet.
    // The body is provided as a view over a buffer that was obtained via our BufferPool.
    void process_packet(const PacketHeader& header, std::string_view body_view);

    // Timeout handling using a single global timer and a min‑heap.
    void schedule_timeout_timer();
    void handle_timeout();

    // Underlying socket and outgoing write queue.
    boost::asio::ip::tcp::socket socket_;
    std::deque<std::vector<char>> write_msgs_;

    // Sequence number generator.
    uint32_t next_sequence_ { 1 };

    // Map of pending response callbacks: sequence -> on_response callback.
    std::unordered_map<uint32_t, std::function<void(std::string_view)>> pending_responses_;

    // Heap used for timer entries.
    TimerHeap timer_heap_;
    // Lookup for quickly erasing a pending timer entry.
    std::unordered_map<uint32_t, TimerHeap::handle_type> pending_handles_;

    // A single global steady timer for processing timeouts.
    std::shared_ptr<boost::asio::steady_timer> timeout_timer_;

    // Object pool for TimerEntry objects.
    boost::object_pool<TimerEntry> timer_pool_;

    // Buffer pool for re‑using read buffers.
    BufferPool readBufferPool_;
};

#endif // SESSION_H
