#include <iostream>
#include <thread>
#include <chrono>
#include <atomic>
#include <vector>
#include <cstdlib>
#include <boost/asio.hpp>
#include "TCPServer.h"
#include "TCPClient.h"

using boost::asio::ip::tcp;

int main(int argc, char* argv[])
{
    if (argc < 2) {
        std::cout << "Usage:\n"
                  << "  Server mode: " << argv[0] << " server [port]\n"
                  << "  Client test mode: " << argv[0] << " client-test <host> <port>\n";
        return 1;
    }

    std::string mode(argv[1]);
    boost::asio::io_context io_context;

    if (mode == "server") {
        short port = (argc >= 3) ? std::atoi(argv[2]) : 12345;
        TCPServer server(io_context, port);
        std::cout << "Server running on port " << port << "\n";
        io_context.run();

    } else if (mode == "client") {
        if (argc < 4) {
            std::cerr << "Usage: " << argv[0] << " client <host> <port>\n";
            return 1;
        }
        std::string host(argv[2]);
        std::string portStr(argv[3]);
        tcp::resolver resolver(io_context);
        auto endpoints = resolver.resolve(host, portStr);

        // Create the TCPClient using our high-performance Session
        auto client = std::make_shared<TCPClient>(io_context, endpoints);
        client->start();

        // Run io_context on a pool of threads to achieve high throughput.
        const int threadCount = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::thread> ioThreads;
        for (int i = 0; i < threadCount; ++i) {
            ioThreads.emplace_back([&io_context]() { io_context.run(); });
        }

        // Set up counters for requests sent and responses received.
        std::atomic<size_t> reqCount{0};
        std::atomic<size_t> respCount{0};

        // Test parameters: run for a fixed duration.
        const auto testDuration = std::chrono::seconds(5); // run test for 5 seconds
        auto testStart = std::chrono::steady_clock::now();

        // In a tight loop, send as many requests as possible.
        // Each request will simply have a small static payload.
        std::string payload = "Test Request";
        std::vector<char> body(payload.begin(), payload.end());

        // The asynchronous callbacks increment counters.
        auto onResponse = [&respCount](std::string_view response) {
            respCount++;
        };
        auto onTimeout = []() {
            // For testing, you might want to log timeout events.
            // std::cerr << "Request timed out\n";
        };

        while (std::chrono::steady_clock::now() - testStart < testDuration) {
            client->send_request(body, 1, onResponse, onTimeout);
            reqCount++;
        }

        // Give a small grace period for pending responses.
        std::this_thread::sleep_for(std::chrono::seconds(2));

        auto testEnd = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(testEnd - testStart).count();
        std::cout << "Test Duration: " << elapsed << " seconds\n";
        std::cout << "Requests Sent: " << reqCount << "\n";
        std::cout << "Responses Received: " << respCount << "\n";
        std::cout << "Throughput: " << static_cast<double>(reqCount) / elapsed << " req/sec\n";

        io_context.stop();
        for (auto &th : ioThreads) {
            if (th.joinable())
                th.join();
        }
    }
    return 0;
}
