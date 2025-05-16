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
#include <chrono>
#include <map>
#include <condition_variable>
#include <future>

// Constants
constexpr int DEFAULT_BUFFER_SIZE = 16384;
constexpr int DEFAULT_RECONNECT_DELAY_MS = 3000;

// Packet header structure (10 bytes total) - matching server implementation
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
using PacketReceivedCallback = std::function<void(const Packet& packet)>;
using ConnectionStatusCallback = std::function<void(bool connected)>;
using ResponseCallback = std::function<void(const Packet& packet)>;
using ResponseTimeoutCallback = std::function<void(uint32_t sequenceNumber)>;

struct PendingRequest {
    std::chrono::steady_clock::time_point expiryTime;
    ResponseCallback callback;
    std::promise<Packet> responsePromise;
};

// Main client class
class TcpClient : public std::enable_shared_from_this<TcpClient> {
public:
    TcpClient(const std::string& host, int port, const std::string& clientId)
        : io_context_(),
          socket_(io_context_),
          work_guard_(asio::make_work_guard(io_context_)),
          strand_(asio::make_strand(io_context_)),
          timer_(io_context_),
          reconnect_timer_(io_context_),
          host_(host),
          port_(port),
          client_id_(clientId),
          connected_(false),
          reconnect_enabled_(false),
          next_sequence_(1),
          read_state_(ReadState::HEADER),
          header_bytes_read_(0),
          payload_bytes_read_(0) {
    }

    ~TcpClient() {
        stop();
    }

    void start() {
        std::cout << "Starting client with ID: " << client_id_ << std::endl;

        // Start the io_context thread
        io_thread_ = std::thread([this]() {
            io_context_.run();
        });

        // Connect to server
        connect();
    }

    void stop() {
        reconnect_enabled_ = false;

        // Cancel any pending timers
        asio::error_code ec;
        timer_.cancel(ec);
        reconnect_timer_.cancel(ec);

        // Close the socket
        close();

        // Stop the io_context and join the thread
        work_guard_.reset();

        if (io_thread_.joinable()) {
            io_thread_.join();
        }

        std::cout << "Client stopped" << std::endl;
    }

    // Set the packet received callback
    void setPacketCallback(PacketReceivedCallback callback) {
        packet_callback_ = std::move(callback);
    }

    // Set the connection status callback
    void setConnectionStatusCallback(ConnectionStatusCallback callback) {
        connection_status_callback_ = std::move(callback);
    }

    // Set the timeout callback
    void setTimeoutCallback(ResponseTimeoutCallback callback) {
        timeout_callback_ = std::move(callback);
    }

    // Send a packet to the server
    bool sendPacket(const Packet& packet) {
        if (!connected_) {
            std::cerr << "Cannot send packet: not connected" << std::endl;
            return false;
        }

        auto self = shared_from_this();
        auto serialized = packet.serialize();

        asio::post(strand_, [this, self, serialized]() {
            bool write_in_progress = !write_queue_.empty();
            write_queue_.push_back(serialized);

            if (!write_in_progress) {
                doWrite();
            }
        });

        return true;
    }

    // Send request and wait for response synchronously
    Packet sendRequestSync(const std::string& data, std::chrono::milliseconds timeout) {
        std::shared_ptr<std::promise<Packet>> promise = std::make_shared<std::promise<Packet>>();
        std::future<Packet> future = promise->get_future();

        uint32_t sequence = sendRequest(data, timeout, [promise](const Packet& packet) {
            promise->set_value(packet);
        });

        if (sequence == 0) {
            // Failed to send request
            throw std::runtime_error("Failed to send request: not connected");
        }

        // Wait for the future with timeout
        auto status = future.wait_for(timeout);
        if (status == std::future_status::timeout) {
            // Cancel the request and throw timeout exception
            cancelRequest(sequence);
            throw std::runtime_error("Request timed out");
        }

        // Get the response
        return future.get();
    }

    // Send a request and register a callback for the response
    uint32_t sendRequest(const std::string& data, std::chrono::milliseconds timeout,
                        ResponseCallback responseCallback) {
        if (!connected_) {
            std::cerr << "Cannot send request: not connected" << std::endl;
            return 0;
        }

        // Generate a new sequence number
        uint32_t sequence = getNextSequence();

        // Create and send the packet
        auto packet = Packet::createPacket(PacketHeader::REQUEST, 0, sequence, data);
        if (!sendPacket(packet)) {
            return 0;
        }

        // Calculate expiry time
        auto expiryTime = std::chrono::steady_clock::now() + timeout;

        // Create a pending request with promise
        std::promise<Packet> responsePromise;

        // Create and register the pending request
        PendingRequest request{
            expiryTime,
            std::move(responseCallback),
            std::move(responsePromise)
        };

        {
            std::lock_guard<std::mutex> lock(requests_mutex_);
            pending_requests_[sequence] = std::move(request);
        }

        // Schedule timeout checking if not already running
        startTimeoutTimer();

        return sequence;
    }

    // Cancel a pending request
    bool cancelRequest(uint32_t sequenceNumber) {
        std::lock_guard<std::mutex> lock(requests_mutex_);
        return pending_requests_.erase(sequenceNumber) > 0;
    }

    // Check if the client is connected
    bool isConnected() const {
        return connected_;
    }

    // Get the client ID
    const std::string& getClientId() const {
        return client_id_;
    }

    // Set whether to automatically reconnect
    void setAutoReconnect(bool enabled) {
        reconnect_enabled_ = enabled;
    }

private:
    enum class ReadState {
        HEADER,
        PAYLOAD
    };

    // Enable auto-reconnect and attempt to connect
    void connect() {
        auto self = shared_from_this();

        asio::post(strand_, [this, self]() {
            // Close the socket if it's open
            close();

            // Reset connection state
            connected_ = false;
            read_state_ = ReadState::HEADER;
            header_bytes_read_ = 0;
            payload_bytes_read_ = 0;

            std::cout << "Connecting to " << host_ << ":" << port_ << "..." << std::endl;

            // Resolve the host name and service
            asio::ip::tcp::resolver resolver(io_context_);
            asio::ip::tcp::resolver::query query(host_, std::to_string(port_));

            resolver.async_resolve(query,
                asio::bind_executor(strand_, [this, self](const asio::error_code& ec,
                                               asio::ip::tcp::resolver::iterator iterator) {
                    if (!ec) {
                        // Connect to the server
                        asio::async_connect(socket_, iterator,
                            asio::bind_executor(strand_, [this, self](const asio::error_code& ec,
                                                         asio::ip::tcp::resolver::iterator) {
                                if (!ec) {
                                    std::cout << "Connected to server" << std::endl;
                                    connected_ = true;

                                    // Notify connection status
                                    notifyConnectionStatus(true);

                                    // Start reading
                                    startRead();

                                    // Send BIND packet
                                    sendBindPacket();
                                } else {
                                    std::cerr << "Connect error: " << ec.message() << std::endl;
                                    scheduleReconnect();
                                }
                            }));
                    } else {
                        std::cerr << "Resolve error: " << ec.message() << std::endl;
                        scheduleReconnect();
                    }
                }));
        });
    }

    // Start reading data from the socket
    void startRead() {
        readHeader();
    }

    // Read the packet header
    void readHeader() {
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
                                current_header_.length > DEFAULT_BUFFER_SIZE) {
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
                        handleDisconnect();
                    }
                }));
    }

    // Read the packet payload
    void readPayload() {
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
                        handleDisconnect();
                    }
                }));
    }

    // Process a complete packet
    void processPacket() {
        // Create complete packet
        Packet packet;
        packet.header = current_header_;
        packet.payload = current_payload_;

        // Print packet info
        std::cout << "Received packet: Type=" << packetTypeToString(packet.header.packetId)
                  << ", Status=" << static_cast<int>(packet.header.status)
                  << ", Seq=" << packet.header.sequence
                  << ", Length=" << packet.header.length << std::endl;

        // If it's a response packet, check for pending requests
        if (packet.header.packetId == PacketHeader::RESPONSE) {
            ResponseCallback callback;

            {
                std::lock_guard<std::mutex> lock(requests_mutex_);
                auto it = pending_requests_.find(packet.header.sequence);

                if (it != pending_requests_.end()) {
                    callback = it->second.callback;
                    pending_requests_.erase(it);
                }
            }

            // Call the callback if found
            if (callback) {
                callback(packet);
            }
        }

        // Handle specific packet types
        switch (packet.header.packetId) {
            case PacketHeader::BIND_RESP: {
                // Extract server ID from payload
                std::string serverId = packet.payload.empty() ?
                    "unknown" : std::string(packet.payload.begin(), packet.payload.end());
                std::cout << "Bound to server: " << serverId << std::endl;
                break;
            }
        }

        // Call the general packet callback
        if (packet_callback_) {
            packet_callback_(packet);
        }
    }

    // Start timeout timer for checking pending requests
    void startTimeoutTimer() {
        auto self = shared_from_this();

        asio::post(strand_, [this, self]() {
            bool hasPendingRequests = false;

            {
                std::lock_guard<std::mutex> lock(requests_mutex_);
                hasPendingRequests = !pending_requests_.empty();
            }

            if (hasPendingRequests) {
                timer_.cancel();
                timer_.expires_after(std::chrono::milliseconds(100));  // Check every 100ms
                timer_.async_wait(asio::bind_executor(strand_,
                    [this, self](const asio::error_code& ec) {
                        if (!ec) {
                            checkTimeouts();
                        }
                    }));
            }
        });
    }

    // Check for timed-out requests
    void checkTimeouts() {
        auto now = std::chrono::steady_clock::now();
        bool hasPending = false;

        std::vector<uint32_t> expiredSequences;

        {
            std::lock_guard<std::mutex> lock(requests_mutex_);

            for (const auto& [sequence, request] : pending_requests_) {
                if (now >= request.expiryTime) {
                    expiredSequences.push_back(sequence);
                }
            }

            // Remove expired requests
            for (const auto& sequence : expiredSequences) {
                pending_requests_.erase(sequence);
            }

            hasPending = !pending_requests_.empty();
        }

        // Call timeout callback for expired requests
        if (!expiredSequences.empty() && timeout_callback_) {
            for (const auto& sequence : expiredSequences) {
                timeout_callback_(sequence);
            }
        }

        // Reschedule timer if we have pending requests
        if (hasPending) {
            timer_.expires_after(std::chrono::milliseconds(100));
            auto self = shared_from_this();
            timer_.async_wait(asio::bind_executor(strand_,
                [this, self](const asio::error_code& ec) {
                    if (!ec) {
                        checkTimeouts();
                    }
                }));
        }
    }

    // Send a bind packet to authenticate with the server
    void sendBindPacket() {
        uint32_t sequence = getNextSequence();
        auto packet = Packet::createPacket(PacketHeader::BIND, 0, sequence, client_id_);
        sendPacket(packet);
    }

    // Write data to the socket
    void doWrite() {
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
                        handleDisconnect();
                    }
                }));
    }

    // Close the socket
    void close() {
        asio::error_code ec;
        if (socket_.is_open()) {
            socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
            socket_.close(ec);
        }
    }

    // Handle a disconnection
    void handleDisconnect() {
        if (connected_) {
            connected_ = false;
            notifyConnectionStatus(false);

            // Clear pending requests with error
            {
                std::lock_guard<std::mutex> lock(requests_mutex_);
                pending_requests_.clear();
            }

            // Schedule reconnect if enabled
            scheduleReconnect();
        }
    }

    // Schedule a reconnection attempt
    void scheduleReconnect() {
        if (!reconnect_enabled_) {
            return;
        }

        auto self = shared_from_this();
        reconnect_timer_.expires_after(std::chrono::milliseconds(DEFAULT_RECONNECT_DELAY_MS));
        reconnect_timer_.async_wait(asio::bind_executor(strand_,
            [this, self](const asio::error_code& ec) {
                if (!ec && reconnect_enabled_) {
                    connect();
                }
            }));
    }

    // Get the next sequence number
    uint32_t getNextSequence() {
        return next_sequence_.fetch_add(1, std::memory_order_relaxed);
    }

    // Notify about connection status change
    void notifyConnectionStatus(bool connected) {
        if (connection_status_callback_) {
            connection_status_callback_(connected);
        }
    }

private:
    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
    asio::strand<asio::io_context::executor_type> strand_;
    asio::steady_timer timer_;
    asio::steady_timer reconnect_timer_;
    std::thread io_thread_;

    std::string host_;
    int port_;
    std::string client_id_;
    bool connected_;
    bool reconnect_enabled_;
    std::atomic<uint32_t> next_sequence_;

    PacketReceivedCallback packet_callback_;
    ConnectionStatusCallback connection_status_callback_;
    ResponseTimeoutCallback timeout_callback_;

    // Reading state variables
    ReadState read_state_;
    size_t header_bytes_read_;
    size_t payload_bytes_read_;
    size_t current_payload_size_;
    PacketHeader current_header_;
    std::vector<uint8_t> current_payload_;
    std::array<uint8_t, sizeof(PacketHeader)> header_buffer_;

    // Writing queue
    std::deque<std::vector<uint8_t>> write_queue_;

    // Pending requests tracking
    std::map<uint32_t, PendingRequest> pending_requests_;
    std::mutex requests_mutex_;
};

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

// Main function
int main(int argc, char* argv[]) {
    try {
        // Default connection parameters
        std::string host = "localhost";
        int port = 8080;
        std::string clientId = "CLIENT_" + std::to_string(std::rand() % 10000);

        // Parse command line arguments
        if (argc >= 2) host = argv[1];
        if (argc >= 3) port = std::stoi(argv[2]);
        if (argc >= 4) clientId = argv[3];

        // Create the client
        auto client = std::make_shared<TcpClient>(host, port, clientId);

        // Set auto-reconnect
        client->setAutoReconnect(true);

        // Set callbacks
        client->setPacketCallback([](const Packet& packet) {
            // Payload as string (if any)
            std::string payloadText;
            if (!packet.payload.empty()) {
                payloadText = std::string(packet.payload.begin(), packet.payload.end());
                std::cout << "  Payload: " << payloadText << std::endl;
            }
        });

        client->setConnectionStatusCallback([](bool connected) {
            std::cout << "Connection status: " << (connected ? "Connected" : "Disconnected") << std::endl;
        });

        client->setTimeoutCallback([](uint32_t sequence) {
            std::cout << "Request with sequence " << sequence << " timed out" << std::endl;
        });

        // Start the client
        client->start();

        std::cout << "Client started. Type 'help' for commands." << std::endl;

        // Command processing loop
        std::string cmd;
        while (true) {
            std::cout << "> ";
            std::getline(std::cin, cmd);

            if (cmd == "quit" || cmd == "exit") {
                break;
            } else if (cmd == "help") {
                std::cout << "Available commands:" << std::endl
                          << "  status                     - Show connection status" << std::endl
                          << "  connect                    - Connect to server" << std::endl
                          << "  disconnect                 - Disconnect from server" << std::endl
                          << "  send <type> <message>      - Send a message (types: bind, req)" << std::endl
                          << "  request <message> <timeout>- Send request and wait for response" << std::endl
                          << "  quit                       - Exit the client" << std::endl;
            } else if (cmd == "status") {
                std::cout << "Client ID: " << client->getClientId() << std::endl;
                std::cout << "Connected: " << (client->isConnected() ? "Yes" : "No") << std::endl;
            } else if (cmd == "connect") {
                client->setAutoReconnect(true);
                std::cout << "Connecting to server..." << std::endl;
                client->start();
            } else if (cmd == "disconnect") {
                client->setAutoReconnect(false);
                client->stop();
                std::cout << "Disconnected from server" << std::endl;
            } else if (cmd.substr(0, 4) == "send" && cmd.length() > 4) {
                // Parse command: send <type> <message>
                std::istringstream iss(cmd.substr(5));
                std::string typeStr, message;

                if (iss >> typeStr) {
                    // Get the rest as message
                    std::getline(iss >> std::ws, message);

                    if (!message.empty()) {
                        uint8_t packetId;
                        uint8_t status = 0;

                        // Parse packet type
                        if (typeStr == "bind") packetId = PacketHeader::BIND;
                        else if (typeStr == "req") packetId = PacketHeader::REQUEST;
                        else {
                            std::cout << "Invalid packet type. Use: bind, req" << std::endl;
                            continue;
                        }

                        // Create and send packet
                        uint32_t sequence = client->getNextSequence();
                        auto packet = Packet::createPacket(packetId, status, sequence, message);

                        if (client->sendPacket(packet)) {
                            std::cout << "Sent " << packetTypeToString(packetId)
                                      << " packet with sequence " << sequence << std::endl;
                        } else {
                            std::cout << "Failed to send packet: not connected" << std::endl;
                        }
                    } else {
                        std::cout << "Message cannot be empty" << std::endl;
                    }
                } else {
                    std::cout << "Invalid command format. Use: send <type> <message>" << std::endl;
                }
            } else if (cmd.substr(0, 7) == "request" && cmd.length() > 7) {
                // Parse command: request <message> <timeout>
                std::istringstream iss(cmd.substr(8));
                std::string message;
                int timeout = 5000; // Default 5 seconds

                // Get the message until the next number (timeout)
                std::string word;
                while (iss >> word) {
                    if (std::isdigit(word[0])) {
                        timeout = std::stoi(word);
                        break;
                    }
                    if (!message.empty()) message += " ";
                    message += word;
                }

                if (!message.empty()) {
                    try {
                        std::cout << "Sending request with timeout " << timeout << "ms: "
                                  << message << std::endl;

                        auto future = std::async(std::launch::async, [&client, &message, timeout]() {
                            try {
                                // Send synchronous request
                                Packet response = client->sendRequestSync(
                                    message, std::chrono::milliseconds(timeout));

                                // Print response
                                std::string responseText;
                                if (!response.payload.empty()) {
                                    responseText = std::string(response.payload.begin(),
                                                              response.payload.end());
                                }

                                std::cout << "Received response for sequence " << response.header.sequence
                                        << ", status " << static_cast<int>(response.header.status)
                                        << ": " << responseText << std::endl;
                            } catch (const std::exception& e) {
                                std::cerr << "Request error: " << e.what() << std::endl;
                            }
                        });

                        // Don't wait for the future, let it complete asynchronously
                    } catch (const std::exception& e) {
                        std::cerr << "Failed to send request: " << e.what() << std::endl;
                    }
                } else {
                    std::cout << "Message cannot be empty" << std::endl;
                }
            } else {
                std::cout << "Unknown command. Type 'help' for available commands." << std::endl;
            }
        }

        // Stop the client gracefully
        std::cout << "Stopping client..." << std::endl;
        client->stop();

    } catch (const std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl
    }
}