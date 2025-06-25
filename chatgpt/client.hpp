#pragma once

#include "connection.hpp"

#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/system/error_code.hpp>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <map>

namespace hpnet {
class Client : public std::enable_shared_from_this<Client> {
public:
    Client(boost::asio::io_context& ioc,
           const boost::asio::ip::tcp::endpoint& ep,
           Config cfg = {});
    ~Client();

    void start();
    void stop();
    void sendRequest(const Packet& pkt,
                     std::function<void(const Packet&)> cb);

    void onBindResp(std::function<void(const Packet&)> cb);
    void onError(ErrorHandler cb);

private:
    void doConnect();
    void scheduleReconnect();
    void scheduleWatchdog();
    void handlePacket(const Packet& pkt);
    void sendBind();
    void scheduleTimeoutWheel();

    boost::asio::ip::tcp::socket socket_;
    boost::asio::io_context& io_context_;
    boost::asio::ip::tcp::endpoint endpoint_;
    boost::asio::steady_timer reconnectTimer_;
    boost::asio::steady_timer watchdogTimer_;
    boost::asio::steady_timer timeoutTimer_;
    Config config_;
    std::atomic<uint32_t> nextSequence_{1};
    std::unordered_map<uint32_t, std::function<void(const Packet&)>> pendingRequests_;
    std::map<std::chrono::steady_clock::time_point, std::vector<uint32_t>> timeoutWheel_;
    std::function<void(const Packet&)> bindHandler_;
    ErrorHandler errorHandler_;
};

} // namespace hpnet

