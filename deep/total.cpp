#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/bind/bind.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/pool/object_pool.hpp>
#include <boost/circular_buffer.hpp>
#include <queue>
#include <unordered_map>
#include <atomic>
#include <memory>

namespace asio = boost::asio;
using asio::ip::tcp;
using namespace std::chrono;

// Fixed protocol header (10 bytes)
#pragma pack(push, 1)
struct PacketHeader {
    uint32_t total_length;  // Includes header + body
    uint8_t packet_type;    // 0=Request, 1=Response
    uint8_t status;
    uint32_t seq_num;
};
#pragma pack(pop)

constexpr size_t HEADER_SIZE = sizeof(PacketHeader);

// Buffer class for zero-copy operations
class Buffer : public boost::enable_shared_from_this<Buffer> {
public:
    using pointer = boost::shared_ptr<Buffer>;

    static pointer create(size_t size) {
        return pointer(new Buffer(size));
    }

    uint8_t* data() { return data_.get(); }
    const uint8_t* data() const { return data_.get(); }
    size_t size() const { return size_; }

    PacketHeader* header() {
        return reinterpret_cast<PacketHeader*>(data_.get());
    }

    uint8_t* body() {
        return data_.get() + HEADER_SIZE;
    }

    size_t body_size() const {
        return size_ - HEADER_SIZE;
    }

    asio::mutable_buffer to_asio_buffer() const {
        return asio::buffer(data_.get(), size_);
    }

private:
    Buffer(size_t size) : size_(size), data_(new uint8_t[size]) {}

    size_t size_;
    std::unique_ptr<uint8_t[]> data_;
};

// Connection class with strand protection
class Connection : public boost::enable_shared_from_this<Connection> {
public:
    using pointer = boost::shared_ptr<Connection>;
    using BufferPtr = Buffer::pointer;
    using PacketCallback = std::function<void(BufferPtr)>;

    Connection(asio::io_context& io, asio::thread_pool& pool)
        : socket_(io),
          strand_(asio::make_strand(io)),
          buffer_pool_(std::make_shared<boost::object_pool<Buffer>>()),
          thread_pool_(pool) {}

    tcp::socket& socket() { return socket_; }

    void start(PacketCallback cb) {
        packet_callback_ = std::move(cb);
        do_read_header();
    }

    void send(BufferPtr buffer) {
        asio::post(strand_, [this, buffer]() {
            bool write_in_progress = !send_queue_.empty();
            send_queue_.push_back(buffer);
            if (!write_in_progress) {
                do_write();
            }
        });
    }

    void close() {
        asio::post(strand_, [this]() {
            if (socket_.is_open()) {
                boost::system::error_code ec;
                socket_.close(ec);
            }
        });
    }

private:
    void do_read_header() {
        auto self = shared_from_this();
        current_read_ = buffer_pool_->construct(HEADER_SIZE);

        asio::async_read(socket_, asio::buffer(current_read_->data(), HEADER_SIZE),
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, size_t bytes) {
                    if (ec) return handle_error(ec);

                    const size_t body_size = current_read_->header()->total_length - HEADER_SIZE;
                    if (body_size > 0) {
                        current_read_ = buffer_pool_->construct(HEADER_SIZE + body_size);
                        std::memcpy(current_read_->data(),
                                   send_queue_.front()->data(),
                                   HEADER_SIZE);
                        do_read_body();
                    } else {
                        dispatch_packet(current_read_);
                        do_read_header();
                    }
                }));
    }

    void do_read_body() {
        auto self = shared_from_this();

        asio::async_read(socket_, asio::buffer(current_read_->body(), current_read_->body_size()),
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, size_t bytes) {
                    if (ec) return handle_error(ec);
                    dispatch_packet(current_read_);
                    do_read_header();
                }));
    }

    void do_write() {
        auto self = shared_from_this();
        auto buffer = send_queue_.front();

        asio::async_write(socket_, buffer->to_asio_buffer(),
            asio::bind_executor(strand_,
                [this, self](boost::system::error_code ec, size_t bytes) {
                    if (ec) return handle_error(ec);

                    send_queue_.pop_front();
                    if (!send_queue_.empty()) {
                        do_write();
                    }
                }));
    }

    void dispatch_packet(BufferPtr buffer) {
        asio::post(thread_pool_,
            [callback = packet_callback_, buffer]() {
                callback(buffer);
            });
    }

    void handle_error(boost::system::error_code ec) {
        close();
        // Reconnect logic would go here
    }

    tcp::socket socket_;
    asio::strand<asio::io_context::executor_type> strand_;
    std::shared_ptr<boost::object_pool<Buffer>> buffer_pool_;
    asio::thread_pool& thread_pool_;
    boost::circular_buffer<BufferPtr> send_queue_{16};
    BufferPtr current_read_;
    PacketCallback packet_callback_;
};

// Connection Pool
class ConnectionPool {
public:
    ConnectionPool(asio::io_context& io, asio::thread_pool& pool,
                  const std::string& host, const std::string& port, size_t size)
        : io_(io), thread_pool_(pool), host_(host), port_(port),
          pool_size_(size), next_index_(0) {}

    void start() {
        for (size_t i = 0; i < pool_size_; ++i) {
            create_connection();
        }
    }

    void send(BufferPtr buffer) {
        auto conn = get_next_connection();
        if (conn) {
            conn->send(buffer);
        } else {
            // Handle no available connections
        }
    }

private:
    void create_connection() {
        auto conn = std::make_shared<Connection>(io_, thread_pool_);
        connections_.push_back(conn);

        tcp::resolver resolver(io_);
        resolver.async_resolve(host_, port_,
            [this, conn](const boost::system::error_code& ec,
                         tcp::resolver::results_type endpoints) {
                if (!ec) {
                    asio::async_connect(conn->socket(), endpoints,
                        [this, conn](const boost::system::error_code& ec,
                                     const tcp::endpoint&) {
                            if (!ec) {
                                conn->start([this](auto buf) { handle_packet(buf); });
                            } else {
                                // Reconnect logic
                            }
                        });
                }
            });
    }

    Connection::pointer get_next_connection() {
        if (connections_.empty()) return nullptr;
        size_t index = next_index_++ % connections_.size();
        return connections_[index];
    }

    void handle_packet(BufferPtr buffer) {
        // Process incoming packets
    }

    asio::io_context& io_;
    asio::thread_pool& thread_pool_;
    std::string host_;
    std::string port_;
    size_t pool_size_;
    std::atomic<size_t> next_index_;
    std::vector<Connection::pointer> connections_;
};

// Timeout Manager using single timer wheel
class TimeoutManager {
public:
    using TimeoutHandler = std::function<void()>;

    TimeoutManager(asio::io_context& io)
        : timer_(io), next_id_(0) {}

    void start() {
        schedule_check();
    }

    uint64_t add_timeout(seconds duration, TimeoutHandler handler) {
        auto id = ++next_id_;
        auto expiry = steady_clock::now() + duration;

        std::lock_guard lock(mutex_);
        timeouts_.push({id, expiry, std::move(handler)});
        return id;
    }

    void cancel_timeout(uint64_t id) {
        std::lock_guard lock(mutex_);
        cancelled_.insert(id);
    }

private:
    struct TimeoutEntry {
        uint64_t id;
        steady_clock::time_point expiry;
        TimeoutHandler handler;

        bool operator>(const TimeoutEntry& other) const {
            return expiry > other.expiry;
        }
    };

    void schedule_check() {
        timer_.expires_after(100ms);
        timer_.async_wait([this](boost::system::error_code ec) {
            if (ec) return;
            check_timeouts();
            schedule_check();
        });
    }

    void check_timeouts() {
        auto now = steady_clock::now();
        std::lock_guard lock(mutex_);

        while (!timeouts_.empty() && timeouts_.top().expiry <= now) {
            auto entry = std::move(timeouts_.top());
            timeouts_.pop();

            if (cancelled_.erase(entry.id) == 0) {
                asio::post(timer_.get_executor(),
                          [handler = std::move(entry.handler)]() { handler(); });
            }
        }
    }

    asio::steady_timer timer_;
    std::mutex mutex_;
    std::priority_queue<TimeoutEntry,
                       std::vector<TimeoutEntry>,
                       std::greater<>> timeouts_;
    std::unordered_set<uint64_t> cancelled_;
    std::atomic<uint64_t> next_id_;
};

// Client class
class Client {
public:
    Client(asio::io_context& io, asio::thread_pool& pool,
          const std::string& host, const std::string& port)
        : pool_(io, pool, host, port, 5),
          timeout_mgr_(io),
          next_seq_(0) {}

    void start() {
        pool_.start();
        timeout_mgr_.start();
    }

    void send_request(BufferPtr request, seconds timeout,
                     std::function<void(BufferPtr)> response_handler,
                     std::function<void()> timeout_handler) {
        const uint32_t seq = ++next_seq_;
        request->header()->seq_num = seq;

        auto timeout_id = timeout_mgr_.add_timeout(timeout, [this, seq, timeout_handler]() {
            if (pending_.erase(seq)) {
                timeout_handler();
            }
        });

        pending_.emplace(seq, PendingRequest{response_handler, timeout_id});
        pool_.send(request);
    }

    void handle_response(BufferPtr response) {
        const uint32_t seq = response->header()->seq_num;
        auto it = pending_.find(seq);
        if (it != pending_.end()) {
            timeout_mgr_.cancel_timeout(it->second.timeout_id);
            it->second.handler(response);
            pending_.erase(it);
        }
    }

private:
    struct PendingRequest {
        std::function<void(BufferPtr)> handler;
        uint64_t timeout_id;
    };

    ConnectionPool pool_;
    TimeoutManager timeout_mgr_;
    std::atomic<uint32_t> next_seq_;
    std::unordered_map<uint32_t, PendingRequest> pending_;
};

// Server class
class Server {
public:
    Server(asio::io_context& io, asio::thread_pool& pool, uint16_t port)
        : acceptor_(io, tcp::endpoint(tcp::v4(), port)),
          thread_pool_(pool) {}

    void start() {
        do_accept();
    }

private:
    void do_accept() {
        acceptor_.async_accept([this](boost::system::error_code ec, tcp::socket socket) {
            if (!ec) {
                auto conn = std::make_shared<Connection>(
                    socket.get_executor().context(), thread_pool_);
                conn->socket() = std::move(socket);
                conn->start([this](auto buf) { handle_request(buf); });
            }
            do_accept();
        });
    }

    void handle_request(BufferPtr request) {
        // Process request and send response
    }

    tcp::acceptor acceptor_;
    asio::thread_pool& thread_pool_;
};

// Usage Example
int main() {
    asio::io_context io;
    asio::thread_pool pool(4); // 4 threads for processing

    // Server
    Server server(io, pool, 12345);
    server.start();

    // Client
    Client client(io, pool, "localhost", "12345");
    client.start();

    // Create request
    auto req = Buffer::create(HEADER_SIZE + 100);
    req->header()->total_length = req->size();
    req->header()->packet_type = 0; // Request
    req->header()->status = 1;

    // Send request
    client.send_request(req, seconds(5),
        [](auto response) { /* handle response */ },
        []() { /* handle timeout */ });

    io.run();
    return 0;
}