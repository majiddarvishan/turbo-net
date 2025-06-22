#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <memory>
#include <functional>
#include <vector>
#include <cstdint>
#include <boost/asio.hpp>
#include "Session.h"

// TCPClient handles outgoing connections to a server. Once connected, it uses the
// underlying Session (with zero‑copy and object pool optimizations) to send requests.
// Responses arrive through the Session’s callbacks.
class TCPClient : public std::enable_shared_from_this<TCPClient> {
public:
    TCPClient(boost::asio::io_context& io_context,
              const boost::asio::ip::tcp::resolver::results_type& endpoints);

    // Begin the connection process. After construction (via shared_ptr),
    // call start() to connect.
    void start();

    // Send a request with the specified body. The underlying session will use
    // a zero‑copy response (std::string_view) and built‑in timeout handling.
    // timeout_seconds: maximum time to wait for a response.
    // on_response: callback invoked with the response body (zero‑copy view).
    // on_timeout: callback invoked if the response never arrives.
    void send_request(const std::vector<char>& body,
                      int timeout_seconds,
                      std::function<void(std::string_view)> on_response,
                      std::function<void()> on_timeout);

private:
    // Initiate connecting to one of the endpoints.
    void start_connect();

    // In case of failure, schedule a reconnect after a short delay.
    void schedule_reconnect();

    // Set up the active session once connected.
    void set_session(std::shared_ptr<Session> session);

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::ip::tcp::resolver::results_type endpoints_;
    std::shared_ptr<Session> session_;
    boost::asio::steady_timer reconnect_timer_;
};

#endif // TCPCLIENT_H
