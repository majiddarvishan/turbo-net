#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <atomic>
#include <boost/asio.hpp>
#include <cstring>
#include <deque>
#include <chrono>
#include <map>

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
using ResponseTimeoutCallback = std::function<void(uint32_t sequenceNumber)>;
using ResponseCallback = std::function<void(int connectionId, const Packet& packet)>;

struct PendingRequest {
    std::chrono::steady_clock::time_point expiryTime;
    int connectionId;
    ResponseCallback callback;
};

// Forward declaration for TcpServer class
class TcpServer {
public:
    TcpServer(int port = DEFAULT_PORT, int numThreads = DEFAULT_THREAD_POOL_SIZE,
              const std::string& serverId = "SERVER_DEFAULT");

    // Get server ID
    const std::string& getServerId() const {
        return server_id_;
    }

    // Set server ID
    void setServerId(const std::string& serverId) {
        server_id_ = serverId;
    }

    void start();
    void setCallbacks(PacketReceivedCallback packetCallback, ConnectionClosedCallback closedCallback);
    uint32_t getNextSequence();
    bool sendPacketTo(int connectionId, const Packet& packet);
    bool sendTo(int connectionId, uint8_t packetId, uint8_t status, const std::string& data);
    void broadcastPacket(const Packet& packet);
    void broadcast(uint8_t packetId, uint8_t status, const std::string& data);
    size_t connectionCount() const;
    std::string getClientInfo(int connectionId) const;
    bool closeConnection(int connectionId);
    void stop();
    void registerConnection(const std::shared_ptr<class Connection>& connection);
    void unregisterConnection(int connectionId);

     uint32_t sendRequest(int connectionId, const std::string& data,
                         std::chrono::milliseconds timeout,
                         ResponseCallback responseCallback,
                         ResponseTimeoutCallback timeoutCallback = nullptr);

    // Set global timeout callback
    void setTimeoutCallback(ResponseTimeoutCallback callback) {
        timeoutCallback_ = std::move(callback);
    }

    // Cancel a pending request
    bool cancelRequest(uint32_t sequenceNumber);

private:
    void startAccept();

    boost::asio::io_context io_context_;
    boost::asio::ip::tcp::acceptor acceptor_;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work_guard_;
    int port_;
    std::atomic<int> nextConnectionId_;
    std::atomic<uint32_t> nextSequence_;
    int thread_pool_size_;
    std::string server_id_;
    std::unordered_map<int, std::shared_ptr<class Connection>> connections_;
    mutable std::mutex mutex_;
    std::vector<std::thread> threads_;

    PacketReceivedCallback packetCallback_;
    ConnectionClosedCallback closedCallback_;

    // Request tracking
    std::map<uint32_t, PendingRequest> pendingRequests_;
    std::mutex requestsMutex_;
    boost::asio::steady_timer timeoutTimer_;
    bool timeoutTimerRunning_ = false;
    ResponseTimeoutCallback timeoutCallback_;
};

// Connection class to handle individual client connections
class Connection : public std::enable_shared_from_this<Connection>
{
public:
    using Pointer = std::shared_ptr<Connection>;

    // Factory method to create a new connection
    static Pointer create(boost::asio::io_context& io_context, TcpServer& server, int id) {
        return Pointer(new Connection(io_context, server, id));
    }

    // Get the socket associated with this connection
    boost::asio::ip::tcp::socket& socket() {
        return socket_;
    }

    // Get connection ID
    int getId() const {
        return id_;
    }

    // Get client ID
    const std::string& getClientId() const {
        return client_id_;
    }

    // Start reading from the connection
    void start(const PacketReceivedCallback& packetCallback,
               const ConnectionClosedCallback& closedCallback);

    // Send a packet to this connection
    void sendPacket(const Packet& packet);

    // Send data with specific packet type
    void sendData(uint8_t packetId, uint8_t status, uint32_t sequence, const std::string& data);

    // Close the connection
    void close();

private:
    // Private constructor - use factory method instead
    Connection(boost::asio::io_context& io_context, TcpServer& server, int id)
        : socket_(io_context),
          strand_(boost::asio::make_strand(io_context)),
          server_(server),
          id_(id),
          client_id_("unknown"), // Initialize client ID
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

    boost::asio::ip::tcp::socket socket_;
    boost::asio::strand<boost::asio::io_context::executor_type> strand_;
    TcpServer& server_;
    int id_;
    std::string client_id_;  // Store client ID from BIND packet
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

// TcpServer implementation
TcpServer::TcpServer(int port, int numThreads, const std::string& serverId)
    : io_context_(),
      acceptor_(io_context_, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), port)),
      port_(port),
      nextConnectionId_(1),
      nextSequence_(1),
      thread_pool_size_(numThreads),
      server_id_(serverId),
      timeoutTimer_(io_context_) {

}

uint32_t TcpServer::sendRequest(int connectionId, const std::string& data,
                              std::chrono::milliseconds timeout,
                              ResponseCallback responseCallback,
                              ResponseTimeoutCallback timeoutCallback) {
    // Generate a new sequence number for this request
    uint32_t sequence = getNextSequence();

    // Create and send the packet
    auto packet = Packet::createPacket(PacketHeader::REQUEST, 0, sequence, data);
    if (!sendPacketTo(connectionId, packet)) {
        // Connection not found, return 0 to indicate failure
        return 0;
    }

    // Calculate expiry time
    auto expiryTime = std::chrono::steady_clock::now() + timeout;

    // Create a pending request entry
    PendingRequest request{
        expiryTime,
        connectionId,
        std::move(responseCallback)
    };

    // Register the pending request with its timeout
    {
        std::lock_guard<std::mutex> lock(requestsMutex_);
        pendingRequests_[sequence] = std::move(request);

        // Set the timeoutCallback if provided
        if (timeoutCallback) {
            timeoutCallback_ = std::move(timeoutCallback);
        }
    }

    // Schedule timeout checking if not already running
    if (!timeoutTimerRunning_) {
        std::lock_guard<std::mutex> lock(requestsMutex_);

        if (!timeoutTimerRunning_) {
            timeoutTimerRunning_ = true;
            timeoutTimer_.expires_after(std::chrono::milliseconds(100));  // Check every 100ms
            timeoutTimer_.async_wait([this](const boost::system::error_code& error) {
                checkTimeouts(error);
            });
        }
    }

    return sequence;
}

// Add method to check for timeouts
void TcpServer::checkTimeouts(const boost::system::error_code& error) {
    if (error) {
        // Timer was cancelled or errored
        std::lock_guard<std::mutex> lock(requestsMutex_);
        timeoutTimerRunning_ = false;
        return;
    }

    auto now = std::chrono::steady_clock::now();
    bool hasPending = false;

    {
        std::lock_guard<std::mutex> lock(requestsMutex_);

        // Use a separate vector to store expired sequence numbers to avoid
        // iterator invalidation when erasing from the map
        std::vector<uint32_t> expiredSequences;

        for (const auto& [sequence, request] : pendingRequests_) {
            if (now >= request.expiryTime) {
                expiredSequences.push_back(sequence);
            }
        }

        // Handle all expired requests
        for (uint32_t sequence : expiredSequences) {
            if (timeoutCallback_) {
                // Call the timeout callback
                timeoutCallback_(sequence);
            }

            // Remove the request
            pendingRequests_.erase(sequence);
        }

        // Check if we have any remaining pending requests
        hasPending = !pendingRequests_.empty();
    }

    // Reschedule the timer if we still have pending requests
    if (hasPending) {
        timeoutTimer_.expires_after(std::chrono::milliseconds(100));
        timeoutTimer_.async_wait([this](const boost::system::error_code& error) {
            checkTimeouts(error);
        });
    } else {
        std::lock_guard<std::mutex> lock(requestsMutex_);
        timeoutTimerRunning_ = false;
    }
}

// Add method to cancel a pending request
bool TcpServer::cancelRequest(uint32_t sequenceNumber) {
    std::lock_guard<std::mutex> lock(requestsMutex_);
    return pendingRequests_.erase(sequenceNumber) > 0;
}

// Add method to handle incoming responses
void TcpServer::handleIncomingPacket(int connectionId, const Packet& packet) {
    // Check if this is a response packet
    if (packet.header.packetId == PacketHeader::RESPONSE) {
        // Check if we have a pending request for this sequence number
        ResponseCallback callback;

        {
            std::lock_guard<std::mutex> lock(requestsMutex_);
            auto it = pendingRequests_.find(packet.header.sequence);

            if (it != pendingRequests_.end()) {
                // Get the callback before erasing
                callback = it->second.callback;

                // Remove the request from pending
                pendingRequests_.erase(it);
            }
        }

        // Call the callback if found
        if (callback) {
            callback(connectionId, packet);
        }
    }

    // Always call the global packet callback if set
    if (packetCallback_) {
        packetCallback_(connectionId, packet);
    }
}

void TcpServer::start() {
    std::cout << "Server starting on port " << port_ << " with "
              << thread_pool_size_ << " threads..." << std::endl;
    std::cout << "Server ID: " << server_id_ << std::endl;

    // Create work guard to keep io_context running
    work_guard_ = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
        boost::asio::make_work_guard(io_context_));

    // Create a shared_ptr to 'this' for use in the connection callbacks
    auto self = shared_from_this();

    // Set up the connection callbacks
    setCallbacks(
        // Packet received callback - redirect through handleIncomingPacket
        [self](int connectionId, const Packet& packet) {
            self->handleIncomingPacket(connectionId, packet);
        },
        // Connection closed callback - use the existing callback
        closedCallback_
    );

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

void TcpServer::setCallbacks(PacketReceivedCallback packetCallback, ConnectionClosedCallback closedCallback) {
    packetCallback_ = std::move(packetCallback);
    closedCallback_ = std::move(closedCallback);
}

uint32_t TcpServer::getNextSequence() {
    return nextSequence_++;
}

bool TcpServer::sendPacketTo(int connectionId, const Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(connectionId);
    if (it != connections_.end()) {
        it->second->sendPacket(packet);
        return true;
    }
    return false;
}

bool TcpServer::sendTo(int connectionId, uint8_t packetId, uint8_t status, const std::string& data) {
    uint32_t sequence = getNextSequence();
    auto packet = Packet::createPacket(packetId, status, sequence, data);
    return sendPacketTo(connectionId, packet);
}

void TcpServer::broadcastPacket(const Packet& packet) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [id, connection] : connections_) {
        connection->sendPacket(packet);
    }
}

void TcpServer::broadcast(uint8_t packetId, uint8_t status, const std::string& data) {
    uint32_t sequence = getNextSequence();
    auto packet = Packet::createPacket(packetId, status, sequence, data);
    broadcastPacket(packet);
}

size_t TcpServer::connectionCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return connections_.size();
}

std::string TcpServer::getClientInfo(int connectionId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(connectionId);
    if (it != connections_.end()) {
        return it->second->getClientId();
    }
    return "Not found";
}

bool TcpServer::closeConnection(int connectionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = connections_.find(connectionId);
    if (it != connections_.end()) {
        it->second->close();
        return true;
    }
    return false;
}

void TcpServer::stop() {
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

void TcpServer::registerConnection(const std::shared_ptr<Connection>& connection) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_[connection->getId()] = connection;
}

void TcpServer::unregisterConnection(int connectionId) {
    std::lock_guard<std::mutex> lock(mutex_);
    connections_.erase(connectionId);

    // Notify using the callback
    if (closedCallback_) {
        closedCallback_(connectionId);
    }
}

void TcpServer::startAccept() {
    int connectionId = nextConnectionId_++;
    auto newConnection = Connection::create(io_context_, *this, connectionId);

    acceptor_.async_accept(newConnection->socket(),
        [this, newConnection](const boost::system::error_code& ec) {
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

// Connection implementation
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

    // Don't send welcome message immediately, wait for BIND packet
    // Instead, just start reading
    readHeader();
}

void Connection::sendPacket(const Packet& packet) {
    auto self = shared_from_this();
    auto serialized = packet.serialize();

    // Use strand to ensure thread safety for write operations
    boost::asio::post(strand_, [self, serialized]() {
        bool write_in_progress = !self->write_queue_.empty();
        self->write_queue_.push_back(serialized);

        if (!write_in_progress) {
            self->doWrite();
        }
    });
}

void Connection::sendData(uint8_t packetId, uint8_t status, uint32_t sequence, const std::string& data) {
    auto packet = Packet::createPacket(packetId, status, sequence, data);
    sendPacket(packet);
}

void Connection::close() {
    // Use strand to ensure thread safety
    boost::asio::post(strand_, [self = shared_from_this()]() {
        if (self->socket_.is_open()) {
            boost::system::error_code ec;
            self->socket_.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
            self->socket_.close(ec);
        }
    });
}

void Connection::readHeader() {
    auto self = shared_from_this();

    boost::asio::async_read(socket_,
        boost::asio::buffer(header_buffer_.data() + header_bytes_read_,
                    sizeof(PacketHeader) - header_bytes_read_),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, std::size_t bytes_transferred) {
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
                } else if (ec != boost::asio::error::operation_aborted) {
                    // Handle read error
                    std::cerr << "Header read error: " << ec.message() << std::endl;
                    server_.unregisterConnection(id_);
                }
            }));
}

void Connection::readPayload() {
    auto self = shared_from_this();

    boost::asio::async_read(socket_,
        boost::asio::buffer(current_payload_.data() + payload_bytes_read_,
                    current_payload_size_ - payload_bytes_read_),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, std::size_t bytes_transferred) {
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
                } else if (ec != boost::asio::error::operation_aborted) {
                    // Handle read error
                    std::cerr << "Payload read error: " << ec.message() << std::endl;
                    server_.unregisterConnection(id_);
                }
            }));
}

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
        case PacketHeader::BIND: {
            // Extract client ID from payload
            client_id_ = (packet.payload.empty()) ?
                "unknown" : std::string(packet.payload.begin(), packet.payload.end());

            std::cout << "Client " << id_ << " identified itself as: " << client_id_ << std::endl;

            // Respond with BIND_RESP containing server ID
            sendData(PacketHeader::BIND_RESP, 0, packet.header.sequence, server_.getServerId());
            break;
        }

        case PacketHeader::REQUEST:
            // For demonstration, echo back the request with RESPONSE
            // In a real application, you'd process the request and generate a proper response
            sendData(PacketHeader::RESPONSE, 0, packet.header.sequence,
                    std::string(packet.payload.begin(), packet.payload.end()));
            break;
    }
}

void Connection::doWrite() {
    auto self = shared_from_this();

    boost::asio::async_write(socket_,
        boost::asio::buffer(write_queue_.front()),
        boost::asio::bind_executor(strand_,
            [this, self](const boost::system::error_code& ec, std::size_t /*length*/) {
                if (!ec) {
                    write_queue_.pop_front();
                    if (!write_queue_.empty()) {
                        doWrite();
                    }
                } else if (ec != boost::asio::error::operation_aborted) {
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
        // Create and configure server with custom ID
        TcpServer server(8080, DEFAULT_THREAD_POOL_SIZE, "SERVER_MAIN_001");

        server->setTimeoutCallback([](uint32_t sequence) {
             std::cout << "Request with sequence " << sequence << " timed out" << std::endl;
        });

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

         // Send request with 5 second timeout
    uint32_t sequence = server->sendRequest(
        clientId,
        requestData,
        std::chrono::seconds(5),
        // Response callback
        [](int connectionId, const Packet& packet) {
            std::cout << "Received response from client " << connectionId
                      << " for sequence " << packet.header.sequence << std::endl;

            // Process the response data
            if (!packet.payload.empty()) {
                std::string responseText(packet.payload.begin(), packet.payload.end());
                std::cout << "Response content: " << responseText << std::endl;
            }
        }
    );

    if (sequence > 0) {
        std::cout << "Request sent with sequence " << sequence << std::endl;
    } else {
        std::cout << "Failed to send request - client not found" << std::endl;
    }

     // Later, you can cancel the request if needed
    if (server->cancelRequest(sequence)) {
        std::cout << "Cancelled request with sequence " << sequence << std::endl;
    }

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
                          << "  clients               - List all connected clients" << std::endl
                          << "  send <id> <type> <msg> - Send message to specific client" << std::endl
                          << "                           Type: bind, bind_resp, req, resp" << std::endl
                          << "  broadcast <type> <msg> - Send message to all clients" << std::endl
                          << "  close <id>            - Close a specific connection" << std::endl
                          << "  count                 - Show number of active connections" << std::endl
                          << "  quit                  - Stop server and exit" << std::endl;
            } else if (cmd == "clients") {
                std::cout << "Connected clients:" << std::endl;
                for (int i = 1; i < server.connectionCount() + 100; i++) {  // Use a large enough range
                    std::string clientId = server.getClientInfo(i);
                    if (clientId != "Not found") {
                        std::cout << "  Connection " << i << ": Client ID = " << clientId << std::endl;
                    }
                }
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