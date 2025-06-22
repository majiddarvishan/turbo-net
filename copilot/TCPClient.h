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

    // Call this method after constructing the client to begin connecting.
    void start();

    // Sends a request with the provided body. Defaults to packet_type 0x01 (request),
    // status 0, and a timeout (in seconds) for waiting the response.
    void send_request(const std::vector<char>& body,
                      uint8_t packet_type = 0x01,
                      uint8_t status = 0,
                      int timeout_seconds = 5);

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
    std::map<uint32_t, std::function<void (const std::vector<char>&)>> pending_responses_;
    std::map<uint32_t, std::shared_ptr<boost::asio::steady_timer>> pending_timers_;
};

#endif // TCPCLIENT_H
