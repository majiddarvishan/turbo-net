#include "TCPServer.h"
#include "PacketHeader.h"
#include <iostream>
#include <memory>
#include <cstring>
using boost::asio::ip::tcp;

TCPServer::TCPServer(boost::asio::io_context& io_context, short port)
    : acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
{
    do_accept();
}

void TCPServer::do_accept() {
    acceptor_.async_accept(
        [this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<Session>(acceptor_.get_executor().context());
                session->socket() = std::move(socket);
                session->start();
                session->on_packet_received = [session](const PacketHeader& header, const std::vector<char>& body) {
                    if (header.packet_type == 0x01) {  // Request packet
                        std::cout << "Server received request with sequence: " << header.sequence << std::endl;
                        // Prepare a response by echoing the request body
                        PacketHeader responseHeader;
                        responseHeader.packet_type = 0x02; // Response
                        responseHeader.status = 0;         // Success status
                        responseHeader.sequence = header.sequence;
                        responseHeader.packet_length = PacketHeader::header_length + static_cast<uint32_t>(body.size());

                        std::vector<char> packet(responseHeader.packet_length);
                        responseHeader.to_buffer(packet.data());
                        if (!body.empty()) {
                            std::memcpy(packet.data() + PacketHeader::header_length, body.data(), body.size());
                        }
                        session->write(packet);
                    }
                };
            }
            do_accept();
        });
}
