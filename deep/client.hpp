#pragma once
#include <boost/asio.hpp>
#include <unordered_map>
#include "connection.hpp"
#include "packet.hpp"

class Client {
public:
    Client(boost::asio::io_context& io_context,
           const std::string& host, const std::string& port);
    void connect();
    void async_request(Packet request,
                      std::function<void(Packet)> response_handler,
                      std::function<void()> timeout_handler,
                      uint32_t timeout_sec);

private:
    void start_connect();
    void handle_connect(const boost::system::error_code& ec);
    void reconnect();
    void check_timeouts();

    boost::asio::io_context& io_context_;
    tcp::resolver resolver_;
    std::string host_;
    std::string port_;
    tcp::socket socket_;
    std::shared_ptr<Connection> connection_;
    boost::asio::steady_timer reconnect_timer_;
    boost::asio::steady_timer timeout_timer_;

    struct PendingRequest {
        std::function<void(Packet)> response_handler;
        std::function<void()> timeout_handler;
        boost::asio::steady_timer timer;
    };

    uint32_t next_seq_ = 0;
    std::unordered_map<uint32_t, PendingRequest> pending_requests_;
};