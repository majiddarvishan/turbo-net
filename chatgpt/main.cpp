#include "high_performance_tcp_lib.hpp"
#include <boost/asio.hpp>
#include <iostream>

int main() {
    boost::asio::io_context io_context;

    // Server setup
    auto server_endpoint = boost::asio::ip::tcp::endpoint(
        boost::asio::ip::address::from_string("127.0.0.1"), 12345);
    hpnet::Server server(io_context, server_endpoint);

    server.onClientConnect([](hpnet::ConnectionPtr conn) {
        std::cout << "Server: Client connected" << std::endl;
        conn->onRequest([conn](const hpnet::Packet& req, hpnet::ConnectionPtr){
            std::string body(req.body.begin(), req.body.end());
            std::cout << "Server received request (seq="
                      << req.header.sequence << "): " << body << std::endl;

            // Echo back as response
            hpnet::Packet resp;
            resp.header.type = hpnet::PacketType::StreamResp;
            resp.header.status = hpnet::Status::Ok;
            resp.header.sequence = req.header.sequence;
            resp.body = req.body;
            resp.header.packet_length = htonl(sizeof(hpnet::Header) + resp.body.size());
            conn->sendPacket(resp);
        });
        conn->onError([](const boost::system::error_code& ec) {
            std::cerr << "Server connection error: " << ec.message() << std::endl;
        });
    });
    server.startAccept();

    // Client setup
    auto client_endpoint = boost::asio::ip::tcp::endpoint(
        boost::asio::ip::address::from_string("127.0.0.1"), 12345);
    auto client = std::make_shared<hpnet::Client>(io_context, client_endpoint);

    client->onBindResp([](const hpnet::Packet& pkt) {
        std::cout << "Client: Bound to server, seq=" << pkt.header.sequence << std::endl;
    });

    client->onError([](const boost::system::error_code& ec) {
        std::cerr << "Client error: " << ec.message() << std::endl;
    });

    client->start();

    // After bind, send a stream request
    io_context.post([client]() {
        hpnet::Packet req;
        req.header.type = hpnet::PacketType::StreamReq;
        req.header.status = hpnet::Status::Ok;
        std::string message = "Hello, server!";
        req.body.assign(message.begin(), message.end());
        req.header.packet_length = htonl(sizeof(hpnet::Header) + req.body.size());

        client->sendRequest(req, [](const hpnet::Packet& resp) {
            std::string body(resp.body.begin(), resp.body.end());
            std::cout << "Client received response (seq="
                      << resp.header.sequence << "): " << body << std::endl;
        });
    });

    io_context.run();
    return 0;
}


