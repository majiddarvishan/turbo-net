#include "TCPServer.h"
#include "PacketHeader.h"
#include <iostream>
#include <memory>
#include <cstring>
using boost::asio::ip::tcp;

TCPServer::TCPServer(boost::asio::io_context& io_context, short port)
    : io_context_(io_context),
      acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
{
    do_accept();
}

void TCPServer::do_accept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<Session>(io_context_);
                session->socket() = std::move(socket);
                session->start();

                // Handle inbound requests from the client.
                session->on_packet_received = [session](const PacketHeader& header, const std::vector<char>& body) {
                    if (header.packet_type == 0x01) {  // Incoming client request.
                        std::cout << "Server received a request with sequence: " << header.sequence << std::endl;
                        // For example, echo the request body back as a response.
                        PacketHeader responseHeader;
                        responseHeader.packet_type = 0x02; // Response.
                        responseHeader.status = 0;         // Indicating success.
                        responseHeader.sequence = header.sequence;
                        responseHeader.packet_length = PacketHeader::header_length + static_cast<uint32_t>(body.size());

                        std::vector<char> packet(responseHeader.packet_length);
                        responseHeader.to_buffer(packet.data());
                        if (!body.empty())
                            std::memcpy(packet.data() + PacketHeader::header_length, body.data(), body.size());

                        session->write(packet);
                    }
                };

                // Now, let the server also initiate a request to the client.
                // For example, ask the client for additional information.
                std::string requestMessage = "Hello from server!";
                std::vector<char> requestBody(requestMessage.begin(), requestMessage.end());
                session->send_request(requestBody, 5,
                    [](const std::vector<char>& response) {
                        std::cout << "Server got response: "
                                  << std::string(response.begin(), response.end()) << std::endl;
                    },
                    [](){
                        std::cerr << "Server's request timed out." << std::endl;
                    }
                );
            }
            do_accept();
        });
}
