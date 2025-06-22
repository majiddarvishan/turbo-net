#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <map>
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>
#include <boost/asio.hpp>
#include "Session.h"

// Structure to store callbacks for a pending request.
struct PendingRequest {
    std::function<void(const std::vector<char>&)> on_response;
    std::function<void()> on_timeout;
};

class TCPClient : public std::enable_shared_from_this<TCPClient> {
public:
    TCPClient(boost::asio::io_context& io_context,
              const boost::asio::ip::tcp::resolver::results_type& endpoints);

    // Call this method after constructing the client (via a shared_ptr)
    // to begin connecting.
    void start();

    // Sends a request that waits up to timeout_seconds for a response.
    // If a response is received before the timeout, on_response is called.
    // Otherwise, on_timeout is invoked.
    //
    // Parameters:
    // - body: the message body to send.
    // - packet_type: type of the packet (default 0x01, meaning "request").
    // - status: any status value (default 0).
    // - timeout_seconds: how many seconds to wait for a response.
    // - on_response: callback when a response is received.
    // - on_timeout: callback when the request times out.
    void send_request(const std::vector<char>& body,
                      uint8_t packet_type = 0x01,
                      uint8_t status = 0,
                      int timeout_seconds = 5,
                      std::function<void(const std::vector<char>&)> on_response = nullptr,
                      std::function<void()> on_timeout = nullptr);

private:
    void start_connect();
    void schedule_reconnect();
    void set_session(std::shared_ptr<Session> session);

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::ip::tcp::resolver::results_type endpoints_;
    std::shared_ptr<Session> session_;
    boost::asio::steady_timer reconnect_timer_;

    uint32_t next_sequence_ { 1 };
    // Maps each request's sequence number to its callbacks.
    std::map<uint32_t, PendingRequest> pending_requests_;
    // Maps each request's sequence number to its timeout timer.
    std::map<uint32_t, std::shared_ptr<boost::asio::steady_timer>> pending_timers_;
};

#endif // TCPCLIENT_H
