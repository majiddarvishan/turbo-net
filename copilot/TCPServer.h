#ifndef TCPSERVER_H
#define TCPSERVER_H

#include <boost/asio.hpp>
#include <memory>
#include "Session.h"

class TCPServer {
public:
    TCPServer(boost::asio::io_context& io_context, short port);

private:
    void do_accept();

    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
};

#endif // TCPSERVER_H
