#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <vector>
#include <deque>
#include <iostream>
#include <mutex>
#include <atomic>
#include <map>
#include <chrono>
#include <functional>
#include <ctime>

using boost::asio::ip::tcp;

// Packet Types
constexpr uint8_t PKT_BIND_REQ = 0x01;
constexpr uint8_t PKT_BIND_RESP = 0x81;
constexpr uint8_t PKT_SUBMIT_REQ = 0x02;
constexpr uint8_t PKT_SUBMIT_RESP = 0x82;
constexpr uint8_t PKT_HEARTBEAT_REQ = 0x03;
constexpr uint8_t PKT_HEARTBEAT_RESP = 0x83;
constexpr uint8_t PKT_UNBIND_REQ = 0x04;
constexpr uint8_t PKT_UNBIND_RESP = 0x84;

// Timeout status
constexpr uint8_t PKT_TIMEOUT_STATUS = 0xFF;

using session_id_type = uint64_t;

struct SessionMetadata {
    std::string ip_address;
    std::time_t connection_time;
    std::time_t last_active_time;
};

using response_callback = std::function<void(const std::vector<uint8_t>&, uint8_t)>;

struct PendingRequest {
    response_callback callback;
    uint8_t expected_type;
    std::shared_ptr<boost::asio::steady_timer> timer;
};

class session : public boost::enable_shared_from_this<session> {
public:
    explicit session(tcp::socket socket)
        : socket_(std::move(socket)),
          strand_(socket_.get_executor()) {}

    void start() {
        do_read_length();
        start_heartbeat();
    }

    void set_session_id(session_id_type id) { session_id_ = id; }
    session_id_type get_session_id() const { return session_id_; }

    void send_packet(uint8_t type, uint8_t status, uint32_t sequence, const std::vector<uint8_t>& payload) {
        uint32_t length = 10 + payload.size();
        std::vector<uint8_t> packet(4 + length);
        *reinterpret_cast<uint32_t*>(packet.data()) = htonl(length);
        packet[4] = type;
        packet[5] = status;
        *reinterpret_cast<uint32_t*>(packet.data() + 6) = htonl(sequence);
        std::copy(payload.begin(), payload.end(), packet.begin() + 10);
        post_write(std::move(packet));
    }

    void graceful_close() {
        std::vector<uint8_t> empty_payload;
        send_packet(PKT_UNBIND_RESP, 0x00, generate_sequence_number(), empty_payload);
        socket_.shutdown(tcp::socket::shutdown_both, [](boost::system::error_code) {});
    }

private:
    tcp::socket socket_;
    boost::asio::io_context::strand strand_;
    session_id_type session_id_;
    enum { length_size = 4 };
    uint8_t length_buffer_[length_size];
    std::vector<uint8_t> message_buffer_;
    std::deque<std::vector<uint8_t>> write_queue_;

    std::shared_ptr<boost::asio::steady_timer> heartbeat_timer_;
    uint32_t last_heartbeat_seq_ = 0;

    void start_heartbeat() {
        heartbeat_timer_ = std::make_shared<boost::asio::steady_timer>(socket_.get_executor());
        heartbeat_timer_->expires_after(std::chrono::seconds(10));
        heartbeat_timer_->async_wait([this](const boost::system::error_code& ec) {
            if (!ec) {
                std::cerr << "Session heartbeat timeout\n";
                if (server_.lock()) {
                    server_.lock()->remove_session(session_id_);
                }
            } else if (ec != boost::asio::error::operation_aborted) {
                std::cerr << "Heartbeat timer error: " << ec.message() << std::endl;
            }
        });

        last_heartbeat_seq_ = generate_sequence_number();
        std::vector<uint8_t> empty_payload;
        send_packet(PKT_HEARTBEAT_REQ, 0x00, last_heartbeat_seq_, empty_payload);
    }

    void do_read_length() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(length_buffer_, length_size),
            boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    uint32_t packet_length = ntohl(*reinterpret_cast<uint32_t*>(length_buffer_));
                    if (packet_length < 10) return;
                    message_buffer_.resize(packet_length - length_size);
                    do_read_body(packet_length);
                } else {
                    if (server_) {
                        auto server = server_.lock();
                        if (server) {
                            server->remove_session(session_id_);
                        }
                    }
                }
            }));
    }

    void do_read_body(uint32_t packet_length) {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(message_buffer_.data(), message_buffer_.size()),
            boost::asio::bind_executor(strand_, [this, self, packet_length](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    handle_packet(packet_length);
                    do_read_length();
                } else {
                    if (server_) {
                        auto server = server_.lock();
                        if (server) {
                            server->remove_session(session_id_);
                        }
                    }
                }
            }));
    }

    void handle_packet(uint32_t packet_length) {
        if (message_buffer_.size() != packet_length - length_size) return;

        uint8_t type = message_buffer_[0];
        uint8_t status = message_buffer_[1];
        uint32_t sequence = ntohl(*reinterpret_cast<uint32_t*>(message_buffer_.data() + 2));
        std::vector<uint8_t> payload(message_buffer_.begin() + 6, message_buffer_.end());

        if (server_) {
            auto server = server_.lock();
            if (server) {
                server->handle_response(this, type, status, sequence, payload);
            }
        }

        switch (type) {
            case PKT_BIND_REQ:
                send_packet(PKT_BIND_RESP, 0x00, sequence, {});
                break;
            case PKT_SUBMIT_REQ:
                send_packet(PKT_SUBMIT_RESP, 0x00, sequence, {});
                break;
            case PKT_HEARTBEAT_REQ:
                send_packet(PKT_HEARTBEAT_RESP, 0x00, sequence, {});
                break;
            case PKT_UNBIND_REQ:
                send_packet(PKT_UNBIND_RESP, 0x00, sequence, {});
                break;
            case PKT_HEARTBEAT_RESP:
                if (sequence == last_heartbeat_seq_) {
                    heartbeat_timer_->cancel();
                    strand_.post([this]() { start_heartbeat(); });
                } else {
                    std::cerr << "Missed heartbeat response: expected " << last_heartbeat_seq_
                              << ", got " << sequence << std::endl;
                }
                break;
        }
    }

    void post_write(std::vector<uint8_t> packet) {
        strand_.dispatch([this, packet = std::move(packet)]() mutable {
            bool should_write = write_queue_.empty();
            write_queue_.push_back(std::move(packet));
            if (should_write) {
                do_write();
            }
        });
    }

    void do_write() {
        strand_.dispatch([this]() {
            if (write_queue_.empty()) return;
            auto& packet = write_queue_.front();
            boost::asio::async_write(socket_, boost::asio::buffer(packet),
                boost::asio::bind_executor(strand_, [this](boost::system::error_code ec, std::size_t) {
                    if (!ec) {
                        write_queue_.pop_front();
                        if (!write_queue_.empty()) {
                            do_write();
                        }
                    } else {
                        if (server_) {
                            auto server = server_.lock();
                            if (server) {
                                server->remove_session(session_id_);
                            }
                        }
                    }
                }));
        });
    }

    friend class server;
    boost::weak_ptr<class server> server_;
};

class server {
public:
    using session_id_type = session_id_type;

    struct SessionStats {
        int active_sessions;
        double avg_session_time;
    };

    server(boost::asio::io_context& io_context, short port,
           std::time_t max_idle_time_seconds = 300)
        : io_context_(io_context),
          acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
          max_idle_time_seconds_(max_idle_time_seconds),
          sequence_generator_(1) {
        start_accept();
        start_expiration_timer();
    }

    void send_request_to_session(session_id_type session_id,
                                 uint8_t request_type,
                                 const std::vector<uint8_t>& payload,
                                 response_callback callback,
                                 uint8_t expected_response_type,
                                 std::chrono::seconds timeout = std::chrono::seconds(5)) {
        uint32_t sequence = generate_sequence_number();

        if (callback && expected_response_type != 0) {
            auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
            timer->expires_after(timeout);

            auto self = shared_from_this();
            timer->async_wait([self, sequence](const boost::system::error_code& ec) {
                if (!ec) {
                    std::lock_guard<std::mutex> lock(self->pending_requests_mutex_);
                    auto it = self->pending_requests_.find(sequence);
                    if (it != self->pending_requests_.end()) {
                        auto& [cb, expected, t] = it->second;
                        cb({}, PKT_TIMEOUT_STATUS);
                        self->pending_requests_.erase(it);
                    }
                }
            });

            std::lock_guard<std::mutex> lock(pending_requests_mutex_);
            pending_requests_[sequence] = {std::move(callback), expected_response_type, std::move(timer)};
        }

        auto session = get_session(session_id);
        if (session) {
            session->send_packet(request_type, 0x00, sequence, payload);
        }
    }

    void send_request_to_all(uint8_t request_type,
                             const std::vector<uint8_t>& payload,
                             response_callback callback,
                             uint8_t expected_response_type,
                             std::chrono::seconds timeout = std::chrono::seconds(5)) {
        uint32_t sequence = generate_sequence_number();

        if (callback && expected_response_type != 0) {
            auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
            timer->expires_after(timeout);

            auto self = shared_from_this();
            timer->async_wait([self, sequence](const boost::system::error_code& ec) {
                if (!ec) {
                    std::lock_guard<std::mutex> lock(self->pending_requests_mutex_);
                    auto it = self->pending_requests_.find(sequence);
                    if (it != self->pending_requests_.end()) {
                        auto& [cb, expected, t] = it->second;
                        cb({}, PKT_TIMEOUT_STATUS);
                        self->pending_requests_.erase(it);
                    }
                }
            });

            std::lock_guard<std::mutex> lock(pending_requests_mutex_);
            pending_requests_[sequence] = {std::move(callback), expected_response_type, std::move(timer)};
        }

        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& pair : sessions_) {
            auto session = pair.second.lock();
            if (session) {
                session->send_packet(request_type, 0x00, sequence, payload);
            }
        }
    }

    session_id_type generate_session_id() {
        static std::atomic<session_id_type> id_counter(1);
        return id_counter++;
    }

    uint32_t generate_sequence_number() {
        return sequence_generator_++;
    }

    void handle_response(session* session, uint8_t type, uint8_t status, uint32_t sequence, const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        auto it = pending_requests_.find(sequence);
        if (it != pending_requests_.end()) {
            auto& [callback, expected_type, timer] = it->second;
            timer->cancel();
            if (expected_type == type) {
                callback(payload, status);
            }
            pending_requests_.erase(it);
        }
    }

    void update_last_active_time(session_id_type id) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = session_metadata_.find(id);
        if (it != session_metadata_.end()) {
            it->second.last_active_time = std::time(nullptr);
        }
    }

    void remove_session(session_id_type id) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            auto session = it->second.lock();
            if (session) {
                session->graceful_close();
            }
            sessions_.erase(it);
        }

        auto meta_it = session_metadata_.find(id);
        if (meta_it != session_metadata_.end()) {
            auto start_time = session_start_times_.find(id);
            if (start_time != session_start_times_.end()) {
                total_session_time_ += std::difftime(std::time(nullptr), start_time->second);
                session_start_times_.erase(start_time);
            }
            session_metadata_.erase(meta_it);
        }

        active_sessions_count_--;
    }

    SessionStats get_statistics() const {
        int active = active_sessions_count_.load();
        double total = total_session_time_.load();
        return {active, active ? total / active : 0.0};
    }

private:
    boost::asio::io_context& io_context_;
    tcp::acceptor acceptor_;
    std::map<session_id_type, boost::weak_ptr<session>> sessions_;
    std::map<session_id_type, SessionMetadata> session_metadata_;
    std::map<session_id_type, std::time_t> session_start_times_;
    std::mutex sessions_mutex_;

    std::atomic<uint32_t> sequence_generator_;
    std::map<uint32_t, PendingRequest> pending_requests_;
    std::mutex pending_requests_mutex_;

    std::time_t max_idle_time_seconds_;
    std::atomic<int> active_sessions_count_{0};
    std::atomic<double> total_session_time_{0.0};

    void start_accept() {
        auto socket = std::make_shared<tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](boost::system::error_code ec) {
            if (!ec) {
                auto new_session = boost::make_shared<session>(std::move(*socket));
                session_id_type session_id = generate_session_id();
                new_session->set_session_id(session_id);
                new_session->server_ = shared_from_this();

                boost::system::error_code ip_ec;
                std::string ip = new_session->socket_.remote_endpoint(ip_ec).address().to_string();
                if (ip_ec) ip = "unknown";

                SessionMetadata metadata;
                metadata.ip_address = ip;
                metadata.connection_time = std::time(nullptr);
                metadata.last_active_time = std::time(nullptr);

                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    sessions_[session_id] = new_session;
                    session_metadata_[session_id] = metadata;
                    session_start_times_[session_id] = metadata.connection_time;
                    active_sessions_count_++;
                }

                new_session->start();
            }
            start_accept();
        });
    }

    void start_expiration_timer() {
        expiration_timer_ = std::make_shared<boost::asio::steady_timer>(io_context_);
        expiration_timer_->expires_after(std::chrono::seconds(30));
        expiration_timer_->async_wait([this](const boost::system::error_code&) {
            check_expired_sessions();
            start_expiration_timer();
        });
    }

    void check_expired_sessions() {
        std::time_t now = std::time(nullptr);
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (auto it = session_metadata_.begin(); it != session_metadata_.end(); ) {
            if (now - it->second.last_active_time > max_idle_time_seconds_) {
                sessions_.erase(it->first);
                auto start_time = session_start_times_.find(it->first);
                if (start_time != session_start_times_.end()) {
                    total_session_time_ += std::difftime(now, start_time->second);
                    session_start_times_.erase(start_time);
                }
                it = session_metadata_.erase(it);
                active_sessions_count_--;
            } else {
                ++it;
            }
        }
    }

    boost::shared_ptr<session> get_session(session_id_type id) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            return it->second.lock();
        }
        return nullptr;
    }

    std::shared_ptr<boost::asio::steady_timer> expiration_timer_;
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: server <port>\n";
            return 1;
        }

        boost::asio::io_context io_context;
        auto server_instance = std::make_shared<server>(
            io_context, std::atoi(argv[1]), 300);

        const int num_threads = 4;
        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&io_context]() {
                io_context.run();
            });
        }

        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            auto stats = server_instance->get_statistics();
            std::cout << "Active Sessions: " << stats.active_sessions << "\n"
                      << "Avg Session Time: " << stats.avg_session_time << "s\n";
        }

        for (auto& t : threads) {
            t.join();
        }

    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}