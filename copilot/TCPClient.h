#ifndef TCPCLIENT_H
#define TCPCLIENT_H

#include <map>
#include <memory>
#include <functional>
#include <vector>
#include <cstdint>
#include <boost/asio.hpp>
#include "Session.h"

class TCPClient : public std::enable_shared_from_this<TCPClient> {
public:
    TCPClient(boost::asio::io_context& io_context,
              const boost::asio::ip::tcp::resolver::results_type& endpoints);

    // Start the connection process. Must be called after constructing the shared_ptr.
    void start();

    // Send a request using the underlying session.
    // - body: the message payload to send.
    // - timeout_seconds: how many seconds to wait for a response.
    // - on_response: called when a response is received.
    // - on_timeout: called if no response is received in time.
    void send_request(const std::vector<char>& body,
                      int timeout_seconds,
                      std::function<void(const std::vector<char>&)> on_response,
                      std::function<void()> on_timeout);

private:
    void start_connect();
    void schedule_reconnect();
    void set_session(std::shared_ptr<Session> session);

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::socket socket_;
    boost::asio::ip::tcp::resolver::results_type endpoints_;
    std::shared_ptr<Session> session_;
    boost::asio::steady_timer reconnect_timer_;
};

#endif // TCPCLIENT_H
