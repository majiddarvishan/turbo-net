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
class Server {
public:
    Server(boost::asio::io_context& ioc,
           const boost::asio::ip::tcp::endpoint& ep,
           Config cfg = {});
    ~Server();

    void startAccept();
    // clientId callback
    void onBind(std::function<void(const std::string&)> cb);
    // send to specific client
    void sendRequest(const std::string& clientId,
                     const Packet& pkt,
                     std::function<void(const Packet&)> cb);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hpnet


