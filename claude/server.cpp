#include <iostream>
#include <cstring>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <unordered_map>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>

// Constants
constexpr int MAX_EVENTS = 64;
constexpr int MAX_BUFFER_SIZE = 4096;
constexpr int DEFAULT_PORT = 8080;
constexpr int DEFAULT_THREAD_POOL_SIZE = 4;

// Forward declarations
class ThreadPool;
class Connection;
class TcpServer;

// Thread Pool for handling connection processing
class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this] {
                while (true) {
                    std::function<void()> task;
                    {
                        std::unique_lock<std::mutex> lock(queueMutex);
                        condition.wait(lock, [this] {
                            return stop || !tasks.empty();
                        });

                        if (stop && tasks.empty()) {
                            return;
                        }

                        task = std::move(tasks.front());
                        tasks.pop();
                    }
                    task();
                }
            });
        }
    }

    template<class F>
    void enqueue(F&& f) {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            if (stop) {
                throw std::runtime_error("ThreadPool is stopped");
            }
            tasks.emplace(std::forward<F>(f));
        }
        condition.notify_one();
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread &worker : workers) {
            worker.join();
        }
    }

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;
    std::mutex queueMutex;
    std::condition_variable condition;
    bool stop;
};

// Connection class to represent a client connection
class Connection {
public:
    explicit Connection(int sockfd) : sockfd(sockfd) {
        // Set socket to non-blocking mode
        int flags = fcntl(sockfd, F_GETFL, 0);
        fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    }

    ~Connection() {
        close(sockfd);
    }

    int getSockfd() const {
        return sockfd;
    }

    // Read data from connection into buffer
    ssize_t readData() {
        ssize_t bytesRead = recv(sockfd, readBuffer.data() + bytesInBuffer,
                                MAX_BUFFER_SIZE - bytesInBuffer, 0);
        if (bytesRead > 0) {
            bytesInBuffer += bytesRead;
        }
        return bytesRead;
    }

    // Send data to connection
    ssize_t sendData(const void* data, size_t length) {
        std::lock_guard<std::mutex> lock(writeMutex);
        ssize_t bytesSent = send(sockfd, data, length, 0);
        return bytesSent;
    }

    // Send data to all connections (broadcast)
    static void broadcast(const std::unordered_map<int, std::shared_ptr<Connection>>& connections,
                         const void* data, size_t length) {
        for (const auto& [fd, conn] : connections) {
            conn->sendData(data, length);
        }
    }

    // Process any data in the buffer
    void processData() {
        // Here you would implement your application protocol
        // For demonstration, we'll just print the received data
        if (bytesInBuffer > 0) {
            readBuffer[bytesInBuffer] = '\0';  // Null-terminate for printing
            std::cout << "Received from fd " << sockfd << ": "
                      << readBuffer.data() << std::endl;

            // Clear buffer after processing
            bytesInBuffer = 0;
        }
    }

private:
    int sockfd;
    std::vector<char> readBuffer = std::vector<char>(MAX_BUFFER_SIZE);
    size_t bytesInBuffer = 0;
    std::mutex writeMutex;
};

// Main TCP Server class
class TcpServer {
public:
    TcpServer(int port = DEFAULT_PORT, int threadPoolSize = DEFAULT_THREAD_POOL_SIZE)
        : port(port), threadPool(threadPoolSize), running(false) {

        // Initialize epoll
        epollFd = epoll_create1(0);
        if (epollFd == -1) {
            throw std::runtime_error("Failed to create epoll instance");
        }

        // Create server socket
        serverFd = socket(AF_INET, SOCK_STREAM, 0);
        if (serverFd == -1) {
            close(epollFd);
            throw std::runtime_error("Failed to create socket");
        }

        // Set socket options
        int opt = 1;
        if (setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            close(serverFd);
            close(epollFd);
            throw std::runtime_error("Failed to set socket options");
        }

        // Set non-blocking mode
        int flags = fcntl(serverFd, F_GETFL, 0);
        if (fcntl(serverFd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(serverFd);
            close(epollFd);
            throw std::runtime_error("Failed to set non-blocking mode");
        }

        // Bind socket
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            close(serverFd);
            close(epollFd);
            throw std::runtime_error("Failed to bind socket");
        }

        // Listen
        if (listen(serverFd, SOMAXCONN) < 0) {
            close(serverFd);
            close(epollFd);
            throw std::runtime_error("Failed to listen on socket");
        }

        // Add server socket to epoll
        epoll_event event{};
        event.events = EPOLLIN;
        event.data.fd = serverFd;
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverFd, &event) < 0) {
            close(serverFd);
            close(epollFd);
            throw std::runtime_error("Failed to add server socket to epoll");
        }
    }

    ~TcpServer() {
        stop();
        close(epollFd);
        close(serverFd);
    }

    // Start the server
    void start() {
        running = true;
        std::cout << "Server started on port " << port << std::endl;

        std::vector<epoll_event> events(MAX_EVENTS);

        while (running) {
            int numEvents = epoll_wait(epollFd, events.data(), MAX_EVENTS, -1);

            for (int i = 0; i < numEvents; ++i) {
                if (events[i].data.fd == serverFd) {
                    // New connection
                    handleNewConnection();
                } else {
                    // Existing connection has data
                    handleExistingConnection(events[i].data.fd, events[i].events);
                }
            }
        }
    }

    // Stop the server
    void stop() {
        running = false;

        // Close all client connections
        std::lock_guard<std::mutex> lock(connectionsMutex);
        connections.clear();
    }

    // Send packet to all connected clients
    void broadcast(const void* data, size_t length) {
        std::lock_guard<std::mutex> lock(connectionsMutex);
        Connection::broadcast(connections, data, length);
    }

private:
    // Handle new client connection
    void handleNewConnection() {
        sockaddr_in clientAddr{};
        socklen_t clientAddrLen = sizeof(clientAddr);

        int clientFd = accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientFd < 0) {
            std::cerr << "Failed to accept connection" << std::endl;
            return;
        }

        // Create connection object
        auto connection = std::make_shared<Connection>(clientFd);

        // Add to epoll
        epoll_event event{};
        event.events = EPOLLIN | EPOLLET;  // Edge-triggered mode
        event.data.fd = clientFd;

        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &event) < 0) {
            std::cerr << "Failed to add client to epoll" << std::endl;
            return;
        }

        // Add to connections map
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            connections[clientFd] = connection;
        }

        std::cout << "New connection from "
                  << inet_ntoa(clientAddr.sin_addr)
                  << ":" << ntohs(clientAddr.sin_port)
                  << " (fd: " << clientFd << ")" << std::endl;

        // Welcome message
        const char* welcomeMsg = "Welcome to the high performance TCP server!\n";
        connection->sendData(welcomeMsg, strlen(welcomeMsg));
    }

    // Handle data from existing connection
    void handleExistingConnection(int fd, uint32_t events) {
        std::shared_ptr<Connection> connection;

        // Get connection from map
        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            auto it = connections.find(fd);
            if (it == connections.end()) {
                return;
            }
            connection = it->second;
        }

        if (events & EPOLLIN) {
            // Data available to read
            threadPool.enqueue([this, connection]() {
                ssize_t bytesRead = connection->readData();

                if (bytesRead > 0) {
                    // Process the data
                    connection->processData();
                } else if (bytesRead == 0) {
                    // Connection closed by client
                    removeConnection(connection->getSockfd());
                } else if (bytesRead < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    // Error
                    std::cerr << "Error reading from socket: " << strerror(errno) << std::endl;
                    removeConnection(connection->getSockfd());
                }
            });
        }

        if (events & (EPOLLERR | EPOLLHUP)) {
            // Error or hang up
            removeConnection(fd);
        }
    }

    // Remove a connection
    void removeConnection(int fd) {
        epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, nullptr);

        {
            std::lock_guard<std::mutex> lock(connectionsMutex);
            connections.erase(fd);
        }

        std::cout << "Connection closed (fd: " << fd << ")" << std::endl;
    }

    int port;
    int serverFd;
    int epollFd;
    std::atomic<bool> running;
    ThreadPool threadPool;
    std::unordered_map<int, std::shared_ptr<Connection>> connections;
    std::mutex connectionsMutex;
};

// Main function with example usage
int main(int argc, char* argv[]) {
    try {
        // Default port is 8080, can be overridden with command line argument
        int port = DEFAULT_PORT;
        if (argc > 1) {
            port = std::stoi(argv[1]);
        }

        // Create and start server
        TcpServer server(port);

        // Example of sending periodic messages to all clients
        std::thread heartbeatThread([&server]() {
            int counter = 0;
            while (true) {
                std::this_thread::sleep_for(std::chrono::seconds(5));

                std::string msg = "Server heartbeat: " + std::to_string(counter++);
                server.broadcast(msg.c_str(), msg.length());
            }
        });

        // Start the server (blocking call)
        server.start();

        // Clean up
        heartbeatThread.join();
    }
    catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}