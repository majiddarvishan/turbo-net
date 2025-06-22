#include <iostream>
#include <thread>
#include <chrono>
#include <boost/asio.hpp>
#include "TCPServer.h"
#include "TCPClient.h"

using boost::asio::ip::tcp;

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            std::cerr << "Usage: " << argv[0] << " <mode> [host] [port]\n";
            std::cerr << "Modes: server or client\n";
            return 1;
        }

        std::string mode = argv[1];
        boost::asio::io_context io_context;

        if (mode == "server") {
            // Default port is 12345 if not provided.
            short port = (argc >= 3) ? std::atoi(argv[2]) : 12345;
            TCPServer server(io_context, port);
            std::cout << "Server is running on port " << port << std::endl;
            io_context.run();
        }
        else if (mode == "client") {
            if (argc < 4) {
                std::cerr << "Usage: " << argv[0] << " client <host> <port>\n";
                return 1;
            }
            std::string host = argv[2];
            std::string port = argv[3];
            tcp::resolver resolver(io_context);
            auto endpoints = resolver.resolve(host, port);
            auto client = std::make_shared<TCPClient>(io_context, endpoints);

            // Run io_context in a background thread.
            std::thread t([&io_context]() { io_context.run(); });

            // Give some time for connection establishment.
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Prepare a sample request message.
            std::string message = "Hello Server!";
            std::vector<char> body(message.begin(), message.end());

            // Send a request. The client will wait for a response for 5 seconds.
            client->send_request(body, 0x01, 0, 5);

            // Wait before shutting down to allow for response processing.
            std::this_thread::sleep_for(std::chrono::seconds(5));
            io_context.stop();
            t.join();
        }
        else {
            std::cerr << "Unknown mode: " << mode << "\n";
            return 1;
        }
    }
    catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
    }
    return 0;
}
