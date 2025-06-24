// example_usage.cpp
#include "high_performance_tcp_lib.hpp"
#include <boost/asio.hpp>
#include <iostream>

int main() {
    boost::asio::io_context io_context;

    auto client_endpoint = boost::asio::ip::tcp::endpoint(
        boost::asio::ip::address::from_string("127.0.0.1"), 12345);
    auto client = std::make_shared<hpnet::Client>(io_context, client_endpoint);
    client->onBindResp([](const hpnet::Packet& pkt) {
        std::cout << "[Client] Bound (seq=" << pkt.header.sequence << ")" << std::endl;
    });
    client->onError([](const boost::system::error_code& ec) {
        std::cerr << "[Client] Error: " << ec.message() << std::endl;
    });
    client->start();

    std::thread t([&](){
        io_context.run();
    });

    // Send a request after binding
    // io_context.post([client]()
    {
        hpnet::Packet req;
        req.header.type = hpnet::PacketType::StreamReq;
        req.header.status = hpnet::Status::Ok;
        std::string msg = "Hello, server!";
        req.body.assign(msg.begin(), msg.end());
        req.header.packet_length = sizeof(hpnet::Header) + req.body.size();
        client->sendRequest(req, [](const hpnet::Packet& resp) {
            std::string body(resp.body.begin(), resp.body.end());
            std::cout << "[Client] Response (seq=" << resp.header.sequence << "): " << body << std::endl;
        });
    }
// );

sleep(50);

int a;
    std::cin >> a;

    return 0;
}
