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
#include <functional>

using boost::asio::ip::tcp;

// Define session ID type
using session_id_type = uint64_t;

class session : public boost::enable_shared_from_this<session> {
public:
    explicit session(tcp::socket socket)
        : socket_(std::move(socket)),
          strand_(socket_.get_executor()) {}

    void start() {
        do_read_length();
    }

    void set_session_id(session_id_type id) {
        session_id_ = id;
    }

    session_id_type get_session_id() const {
        return session_id_;
    }

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

private:
    tcp::socket socket_;
    boost::asio::io_context::strand strand_;
    session_id_type session_id_;
    enum { length_size = 4 };
    uint8_t length_buffer_[length_size];
    std::vector<uint8_t> message_buffer_;
    std::deque<std::vector<uint8_t>> write_queue_;

    void do_read_length() {
        auto self(shared_from_this());
        boost::asio::async_read(socket_, boost::asio::buffer(length_buffer_, length_size),
            boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
                if (!ec) {
                    uint32_t packet_length = ntohl(*reinterpret_cast<uint32_t*>(length_buffer_));
                    if (packet_length < 10) {
                        std::cerr << "Invalid packet length: " << packet_length << std::endl;
                        return;
                    }
                    message_buffer_.resize(packet_length - length_size);
                    do_read_body(packet_length);
                } else {
                    std::cerr << "Read length error: " << ec.message() << std::endl;
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
                    std::cerr << "Read body error: " << ec.message() << std::endl;
                }
            }));
    }

    void handle_packet(uint32_t packet_length) {
        if (message_buffer_.size() != packet_length - length_size) return;

        uint8_t type = message_buffer_[0];
        uint8_t status = message_buffer_[1];
        uint32_t sequence = ntohl(*reinterpret_cast<uint32_t*>(&message_buffer_[2]));
        std::vector<uint8_t> payload(message_buffer_.begin() + 6, message_buffer_.end());

        // Notify server of response
        if (server_) {
            server_->handle_response(this, type, status, sequence, payload);
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
                        std::cerr << "Write error: " << ec.message() << std::endl;
                    }
                }));
        });
    }

    friend class server;
    boost::weak_ptr<class server> server_;
};

class server {
public:
    using response_callback = std::function<void(const std::vector<uint8_t>&, uint8_t)>;
    using session_id_type = session_id_type;

    server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)),
          sequence_generator_(1) {
        start_accept();
    }

    void start_accept() {
        auto socket = std::make_shared<tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](boost::system::error_code ec) {
            if (!ec) {
                auto new_session = boost::make_shared<session>(std::move(*socket));
                session_id_type session_id = generate_session_id();
                new_session->set_session_id(session_id);
                new_session->server_ = shared_from_this();

                {
                    std::lock_guard<std::mutex> lock(sessions_mutex_);
                    sessions_[session_id] = new_session;
                }

                new_session->start();
            }
            start_accept();
        });
    }

    session_id_type generate_session_id() {
        static std::atomic<session_id_type> id_counter(1);
        return id_counter++;
    }

    uint32_t generate_sequence_number() {
        return sequence_generator_++;
    }

    void send_request_to_session(session_id_type session_id,
                                 uint8_t request_type,
                                 const std::vector<uint8_t>& payload,
                                 response_callback callback,
                                 uint8_t expected_response_type) {
        uint32_t sequence = generate_sequence_number();
        if (callback && expected_response_type != 0) {
            std::lock_guard<std::mutex> lock(pending_requests_mutex_);
            pending_requests_[sequence] = std::make_tuple(std::move(callback), expected_response_type);
        }

        auto session = get_session(session_id);
        if (session) {
            session->send_packet(request_type, 0x00, sequence, payload);
        }
    }

    void send_request_to_all(uint8_t request_type,
                             const std::vector<uint8_t>& payload,
                             response_callback callback,
                             uint8_t expected_response_type) {
        uint32_t sequence = generate_sequence_number();
        if (callback && expected_response_type != 0) {
            std::lock_guard<std::mutex> lock(pending_requests_mutex_);
            pending_requests_[sequence] = std::make_tuple(std::move(callback), expected_response_type);
        }

        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& pair : sessions_) {
            auto session = pair.second.lock();
            if (session) {
                session->send_packet(request_type, 0x00, sequence, payload);
            }
        }
    }

    void send_response_to_session(session_id_type session_id,
                                  uint8_t response_type,
                                  uint8_t status,
                                  uint32_t sequence,
                                  const std::vector<uint8_t>& payload) {
        auto session = get_session(session_id);
        if (session) {
            session->send_packet(response_type, status, sequence, payload);
        }
    }

    void handle_response(session* session, uint8_t type, uint8_t status, uint32_t sequence, const std::vector<uint8_t>& payload) {
        std::lock_guard<std::mutex> lock(pending_requests_mutex_);
        auto it = pending_requests_.find(sequence);
        if (it != pending_requests_.end()) {
            auto& [callback, expected_type] = it->second;
            if (expected_type == type) {
                callback(payload, status);
                pending_requests_.erase(it);
            }
        }
    }

private:
    tcp::acceptor acceptor_;
    std::map<session_id_type, boost::weak_ptr<session>> sessions_;
    std::mutex sessions_mutex_;
    std::atomic<uint32_t> sequence_generator_;
    std::map<uint32_t, std::tuple<response_callback, uint8_t>> pending_requests_;
    std::mutex pending_requests_mutex_;

    boost::shared_ptr<session> get_session(session_id_type id) {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            return it->second.lock();
        }
        return nullptr;
    }
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: server <port>\n";
            return 1;
        }

        boost::asio::io_context io_context;
        auto server_instance = std::make_shared<server>(io_context, std::atoi(argv[1]));

        const int num_threads = 4;
        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&io_context]() {
                io_context.run();
            });
        }

        // Example: Send a heartbeat request to a specific session
        std::thread send_thread([server_instance]() {
            std::this_thread::sleep_for(std::chrono::seconds(3));

            session_id_type target_session_id = 123456789; // Replace with actual ID
            std::vector<uint8_t> payload;

            server_instance->send_request_to_session(
                target_session_id,
                0x03, // Heartbeat request
                payload,
                [](const std::vector<uint8_t>& resp, uint8_t status) {
                    std::cout << "Heartbeat response received with status: " << static_cast<int>(status) << std::endl;
                },
                0x83 // Expected heartbeat response
            );
        });

        for (auto& t : threads) {
            t.join();
        }
        send_thread.join();

    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}