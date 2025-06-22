#pragma once
#include <boost/asio.hpp>
#include "connection.hpp"

class Server {
public:
    Server(boost::asio::io_context& io_context, uint16_t port);

    // Set handler for incoming requests
    using RequestHandler = std::function<Packet(Packet)>;
    void set_request_handler(RequestHandler handler);

private:
    void start_accept();
    void handle_accept(std::shared_ptr<Connection> conn,
                       const boost::system::error_code& error);

    tcp::acceptor acceptor_;
    RequestHandler request_handler_;
};