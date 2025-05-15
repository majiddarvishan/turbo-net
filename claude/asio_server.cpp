#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <asio.hpp>

// Constants
constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_BUFFER_SIZE = 4096;
constexpr int DEFAULT_THREAD_POOL_SIZE = 4;

// Forward declarations
class Connection;
class TcpServer;

// Callback types
using DataReceivedCallback = std::function<void(int connectionId, const std::string& data)>;
using ConnectionClosedCallback = std::function<void(int connectionId)>;

// Connection class to handle individual client connections
class Connection : public std::enable_shared_from_this<Connection> {
public:
    using Pointer = std::shared_ptr<Connection>;

    // Factory method to create a new connection
    static Pointer create(asio::io_context& io_context, TcpServer& server, int id) {
        return Pointer(new Connection(io_context, server, id));
    }

    // Get the socket associated with this connection
    asio::ip::tcp::socket& socket() {
        return socket_;
    }

    // Get connection ID
    int getId() const {
        return id_;
    }

    // Start reading from the connection
    void start(const DataReceivedCallback& dataCallback,
               const ConnectionClosedCallback& closedCallback);

    // Send data to this connection
    void sendData(const std::string& message) {
        auto self = shared_from_this();

        // Use strand to ensure thread safety for write operations
        asio::post(strand_, [self, message]() {
            bool write_in_progress = !self->write_queue_.empty();
            self->write_queue_.push_back(message);

            if (!write_in_progress) {
                self->doWrite();
            }
        });
    }

    // Close the connection
    void close() {
        // Use strand to ensure thread safety
        asio::post(strand_, [self = shared_from_this()]() {
            if (self->socket_.is_open()) {
                asio::error_code ec;
                self->socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
                self->socket_.close(ec);
            }
        });
    }

private:
    // Private constructor - use factory method instead
    Connection(asio::io_context& io_context, TcpServer& server, int id)
        : socket_(io_context),
          strand_(asio::make_strand(io_context)),
          server_(server),
          id_(id) {}

    // Read data asynchronously
    void doRead();

    // Write data asynchronously
    void doWrite();

    asio::ip::tcp::socket socket_;
    asio::strand<asio::io_context::executor_type> strand_;
    TcpServer& server_;
    int id_;
    std::array<char, MAX_BUFFER_SIZE> buffer_;
    std::vector<std::string> write_queue_;
    DataReceivedCallback dataCallback_;
    ConnectionClosedCallback closedCallback_;
};

// Main TCP Server class
class TcpServer {
public:
    TcpServer(int port = DEFAULT_PORT, int numThreads = DEFAULT_THREAD_POOL_SIZE)
        : io_context_(),
          acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
          port_(port),
          nextConnectionId_(1),
          thread_pool_size_(numThreads) {
    }

    // Start the server
    void start() {
        std::cout << "Server starting on port " << port_ << " with "
                  << thread_pool_size_ << " threads..." << std::endl;

        // Create work guard to keep io_context running
        work_guard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(io_context_));

        // Start accepting connections
        startAccept();

        // Create thread pool
        for (int i = 0; i < thread_pool_size_; ++i) {
            threads_.emplace_back([this]() {
                io_context_.run();
            });
        }

        std::cout << "Server started successfully" << std::endl;
    }

    // Set callbacks
    void setCallbacks(DataReceivedCallback dataCallback, ConnectionClosedCallback closedCallback) {
        dataCallback_ = std::move(dataCallback);
        closedCallback_ = std::move(closedCallback);
    }

    // Send data to a specific connection
    bool sendTo(int connectionId, const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(connectionId);
        if (it != connections_.end()) {
            it->second->sendData(message);
            return true;
        }
        return false;
    }

    // Send data to all connections
    void broadcast(const std::string& message) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, connection] : connections_) {
            connection->sendData(message);
        }
    }

    // Get number of active connections
    size_t connectionCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return connections_.size();
    }

    // Close a specific connection
    bool closeConnection(int connectionId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(connectionId);
        if (it != connections_.end()) {
            it->second->close();
            return true;
        }
        return false;
    }

    // Stop the server
    void stop() {
        // Stop accepting new connections
        acceptor_.close();

        // Close all connections
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& [id, connection] : connections_) {
                connection->close();
            }
            connections_.clear();
        }

        // Stop the io_context
        work_guard_.reset();

        // Wait for all threads to complete
        for (auto& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        threads_.clear();

        std::cout << "Server stopped" << std::endl;
    }

    // Register a new connection (called internally)
    void registerConnection(const std::shared_ptr<Connection>& connection) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_[connection->getId()] = connection;
    }

    // Unregister a connection (called internally)
    void unregisterConnection(int connectionId) {
        std::lock_guard<std::mutex> lock(mutex_);
        connections_.erase(connectionId);

        // Notify using the callback
        if (closedCallback_) {
            closedCallback_(connectionId);
        }
    }

private:
    // Start accepting new connections
    void startAccept() {
        int connectionId = nextConnectionId_++;
        auto newConnection = Connection::create(io_context_, *this, connectionId);

        acceptor_.async_accept(newConnection->socket(),
            [this, newConnection](const asio::error_code& ec) {
                if (!ec) {
                    // Register and start the connection
                    registerConnection(newConnection);
                    newConnection->start(dataCallback_, closedCallback_);
                } else {
                    std::cerr << "Error accepting connection: " << ec.message() << std::endl;
                }

                // Continue accepting connections if acceptor is still open
                if (acceptor_.is_open()) {
                    startAccept();
                }
            });
    }

    asio::io_context io_context_;
    asio::ip::tcp::acceptor acceptor_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard_;
    int port_;
    std::atomic<int> nextConnectionId_;
    int thread_pool_size_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    mutable std::mutex mutex_;
    std::vector<std::thread> threads_;

    DataReceivedCallback dataCallback_;
    ConnectionClosedCallback closedCallback_;
};

// Implementation of Connection::start
void Connection::start(const DataReceivedCallback& dataCallback,
                       const ConnectionClosedCallback& closedCallback) {
    dataCallback_ = dataCallback;
    closedCallback_ = closedCallback;

    // Get client address info
    try {
        auto endpoint = socket_.remote_endpoint();
        std::cout << "New connection [" << id_ << "] from "
                  << endpoint.address().to_string()
                  << ":" << endpoint.port() << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Error getting remote endpoint: " << e.what() << std::endl;
    }

    // Send welcome message
    sendData("Welcome to the high performance Asio TCP server!\n");

    // Start reading data
    doRead();
}

// Implementation of Connection::doRead
void Connection::doRead() {
    auto self = shared_from_this();

    socket_.async_read_some(asio::buffer(buffer_),
        asio::bind_executor(strand_,
            [this, self](const asio::error_code& ec, std::size_t length) {
                if (!ec) {
                    // Process the received data
                    std::string data(buffer_.data(), length);

                    // Call the data received callback
                    if (dataCallback_) {
                        dataCallback_(id_, data);
                    }

                    // Continue reading
                    doRead();
                } else {
                    // Handle errors
                    if (ec != asio::error::operation_aborted) {
                        server_.unregisterConnection(id_);
                    }
                }
            }));
}

// Implementation of Connection::doWrite
void Connection::doWrite() {
    auto self = shared_from_this();

    asio::async_write(socket_,
        asio::buffer(write_queue_.front()),
        asio::bind_executor(strand_,
            [this, self](const asio::error_code& ec, std::size_t /*length*/) {
                if (!ec) {
                    write_queue_.pop_front();
                    if (!write_queue_.empty()) {
                        doWrite();
                    }
                } else {
                    if (ec != asio::error::operation_aborted) {
                        server_.unregisterConnection(id_);
                    }
                }
            }));
}

// Example usage
int main() {
    try {
        // Create and configure server
        TcpServer server(8080);

        // Set callbacks
        server.setCallbacks(
            // Data received callback
            [](int connectionId, const std::string& data) {
                std::cout << "Received from [" << connectionId << "]: " << data;

                // Echo back
                std::string response = "Echo from server: " + data;
                // Note: In a real application, you might want to call server.sendTo() here
                // But in this example, we handle it in the main loop below
            },

            // Connection closed callback
            [](int connectionId) {
                std::cout << "Connection [" << connectionId << "] closed" << std::endl;
            }
        );

        // Start the server
        server.start();

        // Example of sending periodic heartbeats to all clients
        std::thread heartbeatThread([&server]() {
            int counter = 0;
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::string msg = "Server heartbeat: " + std::to_string(counter++) + "\n";
                server.broadcast(msg);
            }
        });

        // Main application loop
        std::string cmd;
        std::cout << "Server is running. Type 'help' for commands." << std::endl;

        while (true) {
            std::cout << "> ";
            std::getline(std::cin, cmd);

            if (cmd == "quit" || cmd == "exit") {
                break;
            } else if (cmd == "count") {
                std::cout << "Active connections: " << server.connectionCount() << std::endl;
            } else if (cmd == "help") {
                std::cout << "Available commands:" << std::endl
                          << "  send <id> <message> - Send message to specific client" << std::endl
                          << "  broadcast <message> - Send message to all clients" << std::endl
                          << "  close <id>          - Close a specific connection" << std::endl
                          << "  count               - Show number of active connections" << std::endl
                          << "  quit                - Stop server and exit" << std::endl;
            } else if (cmd.substr(0, 4) == "send" && cmd.length() > 4) {
                size_t pos = cmd.find(" ", 5);
                if (pos != std::string::npos) {
                    try {
                        int id = std::stoi(cmd.substr(5, pos - 5));
                        std::string message = cmd.substr(pos + 1) + "\n";

                        if (server.sendTo(id, message)) {
                            std::cout << "Message sent to connection " << id << std::endl;
                        } else {
                            std::cout << "Connection " << id << " not found" << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cout << "Invalid command format. Use: send <id> <message>" << std::endl;
                    }
                } else {
                    std::cout << "Invalid command format. Use: send <id> <message>" << std::endl;
                }
            } else if (cmd.substr(0, 9) == "broadcast" && cmd.length() > 10) {
                std::string message = cmd.substr(10) + "\n";
                server.broadcast(message);
                std::cout << "Message broadcasted to all connections" << std::endl;
            } else if (cmd.substr(0, 5) == "close" && cmd.length() > 6) {
                try {
                    int id = std::stoi(cmd.substr(6));
                    if (server.closeConnection(id)) {
                        std::cout << "Connection " << id << " closed" << std::endl;
                    } else {
                        std::cout << "Connection " << id << " not found" << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cout << "Invalid command format. Use: close <id>" << std::endl;
                }
            } else {
                std::cout << "Unknown command. Type 'help' for available commands." << std::endl;
            }
        }

        // Stop the server gracefully
        std::cout << "Stopping server..." << std::endl;
        server.stop();

        // Need to handle the heartbeat thread properly in a real application
        heartbeatThread.detach();

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}