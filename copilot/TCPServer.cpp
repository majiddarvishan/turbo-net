#include "TCPServer.h"
#include "PacketHeader.h"
#include <iostream>
#include <memory>
#include <cstring>
#include <string>
using boost::asio::ip::tcp;

TCPServer::TCPServer(boost::asio::io_context& io_context, short port)
    : io_context_(io_context),
      acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
{
    do_accept();
}

void TCPServer::do_accept() {
    acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
        if (!ec) {
            // Create a new session for this incoming connection.
            auto session = std::make_shared<Session>(io_context_);
            session->socket() = std::move(socket);
            session->start();

            // Set the callback for packets that are not responses
            // (e.g. client-initiated requests).
            session->on_packet_received = [session](const PacketHeader& header, std::string_view body_view) {
                // Example: if the packet is a client request (packet_type == 0x01),
                // the server echoes back the request as a response.
                if (header.packet_type == 0x01) {
                    std::cout << "Server received request, seq: " << header.sequence
                              << " body: " << std::string(body_view) << "\n";

                    PacketHeader responseHeader;
                    responseHeader.packet_type = 0x02; // Response packet.
                    responseHeader.status = 0;         // Success.
                    responseHeader.sequence = header.sequence;
                    responseHeader.packet_length = PacketHeader::header_length +
                        static_cast<uint32_t>(body_view.size());

                    std::vector<char> packet(responseHeader.packet_length);
                    responseHeader.to_buffer(packet.data());
                    if (!body_view.empty()) {
                        std::memcpy(packet.data() + PacketHeader::header_length,
                                    body_view.data(),
                                    body_view.size());
                    }
                    // Send the response back to the client.
                    session->write(packet);
                }
                else {
                    std::cout << "Server received packet with unknown type, seq: "
                              << header.sequence << "\n";
                }
            };

            // Server-initiated request example:
            // After connection is established, the server sends a request to the client.
            std::string serverRequest = "Hello from server!";
            std::vector<char> reqBody(serverRequest.begin(), serverRequest.end());
            session->send_request(reqBody,
                5, // timeout in seconds
                [](std::string_view response) {
                    std::cout << "Server received response from client: "
                              << std::string(response) << "\n";
                },
                []() {
                    std::cerr << "Server's request timed out.\n";
                }
            );
        }
        else {
            std::cerr << "Accept error: " << ec.message() << "\n";
        }
        // Accept the next connection.
        do_accept();
    });
}
