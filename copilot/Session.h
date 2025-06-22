#ifndef SESSION_H
#define SESSION_H

#include <deque>
#include <vector>
#include <memory>
#include <functional>
#include <chrono>
#include <unordered_map>
#include <boost/asio.hpp>
#include <boost/heap/fibonacci_heap.hpp>
#include <boost/pool/object_pool.hpp>
#include "PacketHeader.h"
#include "TimerEntry.h"

// Define a Fibonacci heap that holds TimerEntry pointers.
using TimerHeap = boost::heap::fibonacci_heap<TimerEntry*, boost::heap::compare<TimerEntryPtrCompare>>;

class Session : public std::enable_shared_from_this<Session> {
public:
    explicit Session(boost::asio::io_context& io_context);

    // Access to the underlying socket
    boost::asio::ip::tcp::socket& socket();

    // Start the session (begin asynchronous reads, etc.)
    void start();

    // Queue up data to be sent.
    void write(const std::vector<char>& data);

    // Generic callback for packets that are not responses to a send_request.
    std::function<void(const PacketHeader&, const std::vector<char>&)> on_packet_received;

    // Send a request with the given payload. The method waits up to timeout_seconds
    // for a response. If a response arrives (packet_type == 0x02 with matching sequence),
    // on_response is called; otherwise, on_timeout is invoked.
    void send_request(const std::vector<char>& body,
                      int timeout_seconds,
                      std::function<void(const std::vector<char>&)> on_response,
                      std::function<void()> on_timeout);

private:
    // Internal asynchronous reading methods.
    void do_read_header();
    void do_read_body(const PacketHeader& header, std::size_t body_length);

    // Internal asynchronous write method.
    void do_write();

    // Process a received complete packet.
    void process_packet(const PacketHeader& header, const std::vector<char>& body);

    // Timeout handling: schedule the global timer to fire at the earliest pending deadline.
    void schedule_timeout_timer();
    // Callback invoked when the global timeout timer expires.
    void handle_timeout();

    // Underlying TCP connection
    boost::asio::ip::tcp::socket socket_;
    std::deque<std::vector<char>> write_msgs_;
    std::vector<char> read_buffer_;

    // Used to generate unique sequence numbers.
    uint32_t next_sequence_ { 1 };

    // Mapping from sequence numbers to on_response callbacks.
    std::unordered_map<uint32_t, std::function<void(const std::vector<char>&)>> pending_responses_;

    // Heap to hold pending timer entries (by pointer).
    TimerHeap timer_heap_;
    // For quick lookup of the heap handle for a given sequence.
    std::unordered_map<uint32_t, TimerHeap::handle_type> pending_handles_;

    // A single steady timer used to check for expired requests.
    std::shared_ptr<boost::asio::steady_timer> timeout_timer_;

    // Object pool to allocate TimerEntry objects (reducing per-request allocations).
    boost::object_pool<TimerEntry> timer_pool_;
};

#endif // SESSION_H
