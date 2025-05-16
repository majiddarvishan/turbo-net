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
#include <cstring>
#include <deque>

// Constants
constexpr int DEFAULT_PORT = 8080;
constexpr int MAX_BUFFER_SIZE = 16384; // Larger buffer to accommodate packets
constexpr int DEFAULT_THREAD_POOL_SIZE = 4;

// Packet header structure (10 bytes total)
struct PacketHeader {
    uint32_t length;   // 4 bytes: total packet length including header
    uint8_t  packetId; // 1 byte: packet type ID
    uint8_t  status;   // 1 byte: status code
    uint32_t sequence; // 4 bytes: sequence number

    // Packet type constants
    static constexpr uint8_t BIND = 0x01;
    static constexpr uint8_t BIND_RESP = 0x81;
    static constexpr uint8_t REQUEST = 0x02;
    static constexpr uint8_t RESPONSE = 0x82;

    // Convert header to network byte order for sending
    void toNetworkByteOrder() {
        length = htonl(length);
        sequence = htonl(sequence);
    }

    // Convert header from network to host byte order after receiving
    void toHostByteOrder() {
        length = ntohl(length);
        sequence = ntohl(sequence);
    }
};

// Forward declarations
class TcpServer;

// Define a Packet structure to include header and payload
struct Packet {
    PacketHeader header;
    std::vector<uint8_t> payload;

    static Packet createPacket(uint8_t packetId, uint8_t status, uint32_t sequence,
                              const std::vector<uint8_t>& payload) {
        Packet packet;
        packet.header.length = sizeof(PacketHeader) + payload.size();
        packet.header.packetId = packetId;
        packet.header.status = status;
        packet.header.sequence = sequence;
        packet.payload = payload;
        return packet;
    }

    // Convert string data to packet payload
    static Packet createPacket(uint8_t packetId, uint8_t status, uint32_t sequence,
                              const std::string& data) {
        std::vector<uint8_t> payload(data.begin(), data.end());
        return createPacket(packetId, status, sequence, payload);
    }

    // Serialize packet to binary format for sending
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer(sizeof(PacketHeader) + payload.size());

        // Copy header with network byte order
        PacketHeader networkHeader = header;
        networkHeader.toNetworkByteOrder();
        std::memcpy(buffer.data(), &networkHeader, sizeof(PacketHeader));

        // Copy payload
        if (!payload.empty()) {
            std::memcpy(buffer.data() + sizeof(PacketHeader), payload.data(), payload.size());
        }

        return buffer;
    }
};

// Callback types
using PacketReceivedCallback = std::function<void(int connectionId, const Packet& packet)>;
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
    void start(const PacketReceivedCallback& packetCallback,
               const ConnectionClosedCallback& closedCallback);

    // Send a packet to this connection
    void sendPacket(const Packet& packet) {
        auto self = shared_from_this();
        auto serialized = packet.serialize();

        // Use strand to ensure thread safety for write operations
        asio::post(strand_, [self, serialized]() {
            bool write_in_progress = !self->write_queue_.empty();
            self->write_queue_.push_back(serialized);

            if (!write_in_progress) {
                self->doWrite();
            }
        });
    }

    // Send data with specific packet type
    void sendData(uint8_t packetId, uint8_t status, uint32_t sequence, const std::string& data) {
        auto packet = Packet::createPacket(packetId, status, sequence, data);
        sendPacket(packet);
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
          id_(id),
          read_state_(ReadState::HEADER),
          header_bytes_read_(0),
          payload_bytes_read_(0),
          current_payload_size_(0) {}

    // Read header asynchronously
    void readHeader();

    // Read payload asynchronously
    void readPayload();

    // Process a complete packet
    void processPacket();

    // Write data asynchronously
    void doWrite();

    // Enum for the connection read state
    enum class ReadState {
        HEADER,
        PAYLOAD
    };

    asio::ip::tcp::socket socket_;
    asio::strand<asio::io_context::executor_type> strand_;
    TcpServer& server_;
    int id_;
    std::deque<std::vector<uint8_t>> write_queue_;
    PacketReceivedCallback packetCallback_;
    ConnectionClosedCallback closedCallback_;

    // Reading state variables
    ReadState read_state_;
    size_t header_bytes_read_;
    size_t payload_bytes_read_;
    size_t current_payload_size_;
    PacketHeader current_header_;
    std::vector<uint8_t> current_payload_;
    std::array<uint8_t, sizeof(PacketHeader)> header_buffer_;
};

// Main TCP Server class
class TcpServer {
public:
    TcpServer(int port = DEFAULT_PORT, int numThreads = DEFAULT_THREAD_POOL_SIZE)
        : io_context_(),
          acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
          port_(port),
          nextConnectionId_(1),
          nextSequence_(1),
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
    void setCallbacks(PacketReceivedCallback packetCallback, ConnectionClosedCallback closedCallback) {
        packetCallback_ = std::move(packetCallback);
        closedCallback_ = std::move(closedCallback);
    }

    // Get next sequence number
    uint32_t getNextSequence() {
        return nextSequence_++;
    }

    // Send a packet to a specific connection
    bool sendPacketTo(int connectionId, const Packet& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = connections_.find(connectionId);
        if (it != connections_.end()) {
            it->second->sendPacket(packet);
            return true;
        }
        return false;
    }

    // Send data to a specific connection with auto-generated sequence
    bool sendTo(int connectionId, uint8_t packetId, uint8_t status, const std::string& data) {
        uint32_t sequence = getNextSequence();
        auto packet = Packet::createPacket(packetId, status, sequence, data);
        return sendPacketTo(connectionId, packet);
    }

    // Broadcast a packet to all connections
    void broadcastPacket(const Packet& packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [id, connection] : connections_) {
            connection->sendPacket(packet);
        }
    }

    // Broadcast data to all connections with auto-generated sequence
    void broadcast(uint8_t packetId, uint8_t status, const std::string& data) {
        uint32_t sequence = getNextSequence();
        auto packet = Packet::createPacket(packetId, status, sequence, data);
        broadcastPacket(packet);
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
                    newConnection->start(packetCallback_, closedCallback_);
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
    std::atomic<uint32_t> nextSequence_;
    int thread_pool_size_;
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    mutable std::mutex mutex_;
    std::vector<std::thread> threads_;

    PacketReceivedCallback packetCallback_;
    ConnectionClosedCallback closedCallback_;
};

// Implementation of Connection::start
void Connection::start(const PacketReceivedCallback& packetCallback,
                       const ConnectionClosedCallback& closedCallback) {
    packetCallback_ = packetCallback;
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

    // Send welcome message as a BIND_RESP packet
    std::string welcomeMsg = "Welcome to the high performance Asio TCP server!";
    sendData(PacketHeader::BIND_RESP, 0, server_.getNextSequence(), welcomeMsg);

    // Start reading header
    readHeader();
}

// Implementation of Connection::readHeader
void Connection::readHeader() {
    auto self = shared_from_this();

    asio::async_read(socket_,
        asio::buffer(header_buffer_.data() + header_bytes_read_,
                    sizeof(PacketHeader) - header_bytes_read_),
        asio::bind_executor(strand_,
            [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
                if (!ec) {
                    header_bytes_read_ += bytes_transferred;

                    if (header_bytes_read_ == sizeof(PacketHeader)) {
                        // We've read the complete header
                        std::memcpy(&current_header_, header_buffer_.data(), sizeof(PacketHeader));
                        current_header_.toHostByteOrder();

                        // Validate header
                        if (current_header_.length < sizeof(PacketHeader) ||
                            current_header_.length > MAX_BUFFER_SIZE) {
                            std::cerr << "Invalid packet length: " << current_header_.length << std::endl;
                            close();
                            return;
                        }

                        // Calculate payload size
                        current_payload_size_ = current_header_.length - sizeof(PacketHeader);

                        if (current_payload_size_ > 0) {
                            // Prepare for payload
                            current_payload_.resize(current_payload_size_);
                            payload_bytes_read_ = 0;
                            read_state_ = ReadState::PAYLOAD;

                            // Start reading payload
                            readPayload();
                        } else {
                            // No payload, process the packet now
                            processPacket();

                            // Reset for next header
                            header_bytes_read_ = 0;
                            read_state_ = ReadState::HEADER;
                            readHeader();
                        }
                    } else {
                        // Continue reading header
                        readHeader();
                    }
                } else if (ec != asio::error::operation_aborted) {
                    // Handle read error
                    std::cerr << "Header read error: " << ec.message() << std::endl;
                    server_.unregisterConnection(id_);
                }
            }));
}

// Implementation of Connection::readPayload
void Connection::readPayload() {
    auto self = shared_from_this();

    asio::async_read(socket_,
        asio::buffer(current_payload_.data() + payload_bytes_read_,
                    current_payload_size_ - payload_bytes_read_),
        asio::bind_executor(strand_,
            [this, self](const asio::error_code& ec, std::size_t bytes_transferred) {
                if (!ec) {
                    payload_bytes_read_ += bytes_transferred;

                    if (payload_bytes_read_ == current_payload_size_) {
                        // Complete packet received, process it
                        processPacket();

                        // Reset for next header
                        header_bytes_read_ = 0;
                        read_state_ = ReadState::HEADER;
                        readHeader();
                    } else {
                        // Continue reading payload
                        readPayload();
                    }
                } else if (ec != asio::error::operation_aborted) {
                    // Handle read error
                    std::cerr << "Payload read error: " << ec.message() << std::endl;
                    server_.unregisterConnection(id_);
                }
            }));
}

// Implementation of Connection::processPacket
void Connection::processPacket() {
    // Create complete packet
    Packet packet;
    packet.header = current_header_;
    packet.payload = current_payload_;

    // Call the packet received callback
    if (packetCallback_) {
        packetCallback_(id_, packet);
    }

    // Handle specific packet types automatically
    switch (packet.header.packetId) {
        case PacketHeader::BIND:
            // Auto-respond to BIND with BIND_RESP
            sendData(PacketHeader::BIND_RESP, 0, packet.header.sequence, "Bind successful");
            break;

        case PacketHeader::REQUEST:
            // For demonstration, echo back the request with RESPONSE
            // In a real application, you'd process the request and generate a proper response
            sendData(PacketHeader::RESPONSE, 0, packet.header.sequence,
                    std::string(packet.payload.begin(), packet.payload.end()));
            break;
    }
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
                } else if (ec != asio::error::operation_aborted) {
                    std::cerr << "Write error: " << ec.message() << std::endl;
                    server_.unregisterConnection(id_);
                }
            }));
}

// Utility function to print packet information
std::string packetTypeToString(uint8_t packetId) {
    switch (packetId) {
        case PacketHeader::BIND: return "BIND";
        case PacketHeader::BIND_RESP: return "BIND_RESP";
        case PacketHeader::REQUEST: return "REQUEST";
        case PacketHeader::RESPONSE: return "RESPONSE";
        default: return "UNKNOWN";
    }
}

// Example usage
int main() {
    try {
        // Create and configure server
        TcpServer server(8080);

        // Set callbacks
        server.setCallbacks(
            // Packet received callback
            [](int connectionId, const Packet& packet) {
                std::cout << "Received packet from [" << connectionId << "]: "
                          << "Type=" << packetTypeToString(packet.header.packetId)
                          << ", Status=" << static_cast<int>(packet.header.status)
                          << ", Seq=" << packet.header.sequence
                          << ", Length=" << packet.header.length << std::endl;

                // If packet has payload, print it as text
                if (!packet.payload.empty()) {
                    std::string text(packet.payload.begin(), packet.payload.end());
                    std::cout << "  Payload: " << text << std::endl;
                }
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
                std::string msg = "Server heartbeat: " + std::to_string(counter++);
                server.broadcast(PacketHeader::RESPONSE, 0, msg);
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
                          << "  send <id> <type> <msg> - Send message to specific client" << std::endl
                          << "                           Type: bind, bind_resp, req, resp" << std::endl
                          << "  broadcast <type> <msg> - Send message to all clients" << std::endl
                          << "  close <id>            - Close a specific connection" << std::endl
                          << "  count                 - Show number of active connections" << std::endl
                          << "  quit                  - Stop server and exit" << std::endl;
            } else if (cmd.substr(0, 4) == "send" && cmd.length() > 4) {
                // Parse command: send <id> <type> <message>
                std::istringstream iss(cmd.substr(5));
                std::string idStr, typeStr, message;

                if (iss >> idStr >> typeStr) {
                    // Get the rest as message
                    std::getline(iss >> std::ws, message);

                    if (!message.empty()) {
                        try {
                            int id = std::stoi(idStr);
                            uint8_t packetId = PacketHeader::RESPONSE; // Default

                            // Parse packet type
                            if (typeStr == "bind") packetId = PacketHeader::BIND;
                            else if (typeStr == "bind_resp") packetId = PacketHeader::BIND_RESP;
                            else if (typeStr == "req") packetId = PacketHeader::REQUEST;
                            else if (typeStr == "resp") packetId = PacketHeader::RESPONSE;
                            else {
                                std::cout << "Invalid packet type. Use: bind, bind_resp, req, resp" << std::endl;
                                continue;
                            }

                            if (server.sendTo(id, packetId, 0, message)) {
                                std::cout << "Sent " << packetTypeToString(packetId)
                                          << " packet to connection " << id << std::endl;
                            } else {
                                std::cout << "Connection " << id << " not found" << std::endl;
                            }
                        } catch (const std::exception& e) {
                            std::cout << "Invalid command format. Use: send <id> <type> <message>" << std::endl;
                        }
                    } else {
                        std::cout << "Message cannot be empty" << std::endl;
                    }
                } else {
                    std::cout << "Invalid command format. Use: send <id> <type> <message>" << std::endl;
                }
            } else if (cmd.substr(0, 9) == "broadcast" && cmd.length() > 9) {
                // Parse command: broadcast <type> <message>
                std::istringstream iss(cmd.substr(10));
                std::string typeStr, message;

                if (iss >> typeStr) {
                    // Get the rest as message
                    std::getline(iss >> std::ws, message);

                    if (!message.empty()) {
                        uint8_t packetId = PacketHeader::RESPONSE; // Default

                        // Parse packet type
                        if (typeStr == "bind") packetId = PacketHeader::BIND;
                        else if (typeStr == "bind_resp") packetId = PacketHeader::BIND_RESP;
                        else if (typeStr == "req") packetId = PacketHeader::REQUEST;
                        else if (typeStr == "resp") packetId = PacketHeader::RESPONSE;
                        else {
                            std::cout << "Invalid packet type. Use: bind, bind_resp, req, resp" << std::endl;
                            continue;
                        }

                        server.broadcast(packetId, 0, message);
                        std::cout << "Broadcasted " << packetTypeToString(packetId)
                                  << " packet to all connections" << std::endl;
                    } else {
                        std::cout << "Message cannot be empty" << std::endl;
                    }
                } else {
                    std::cout << "Invalid command format. Use: broadcast <type> <message>" << std::endl;
                }
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