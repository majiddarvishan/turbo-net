#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <vector>
#include <deque>
#include <iostream>

using boost::asio::ip::tcp;

class session : public boost::enable_shared_from_this<session> {
public:
    explicit session(tcp::socket socket)
        : socket_(std::move(socket)),
          strand_(socket_.get_executor()) {}

    void start() {
        do_read_length();
    }

private:
    tcp::socket socket_;
    boost::asio::io_context::strand strand_;

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

        switch (type) {
            case 0x01:
                handle_bind_request(status, sequence);
                break;
            case 0x02:
                handle_submit_request(status, sequence, payload);
                break;
            case 0x03:
                handle_heartbeat_request(sequence);
                break;
            case 0x04:
                handle_unbind_request(sequence);
                break;
            default:
                std::cerr << "Unknown packet type: " << static_cast<int>(type) << std::endl;
        }
    }

    void handle_bind_request(uint8_t /*status*/, uint32_t sequence) {
        std::vector<uint8_t> empty_payload;
        send_packet(0x81, 0x00, sequence, empty_payload);
    }

    void handle_submit_request(uint8_t /*status*/, uint32_t sequence, const std::vector<uint8_t>& /*payload*/) {
        std::vector<uint8_t> empty_payload;
        send_packet(0x82, 0x00, sequence, empty_payload);
    }

    void handle_heartbeat_request(uint32_t sequence) {
        std::vector<uint8_t> empty_payload;
        send_packet(0x83, 0x00, sequence, empty_payload);
    }

    void handle_unbind_request(uint32_t sequence) {
        std::vector<uint8_t> empty_payload;
        send_packet(0x84, 0x00, sequence, empty_payload);
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
};

class server {
public:
    server(boost::asio::io_context& io_context, short port)
        : acceptor_(io_context, tcp::endpoint(tcp::v4(), port)) {
        start_accept();
    }

private:
    tcp::acceptor acceptor_;

    void start_accept() {
        auto socket = std::make_shared<tcp::socket>(acceptor_.get_executor());
        acceptor_.async_accept(*socket, [this, socket](boost::system::error_code ec) {
            if (!ec) {
                boost::make_shared<session>(std::move(*socket))->start();
            }
            start_accept();
        });
    }
};

int main(int argc, char* argv[]) {
    try {
        if (argc != 2) {
            std::cerr << "Usage: server <port>\n";
            return 1;
        }

        boost::asio::io_context io_context;
        server s(io_context, std::atoi(argv[1]));

        const int num_threads = 4;
        std::vector<std::thread> threads;
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&io_context]() {
                io_context.run();
            });
        }

        for (auto& t : threads) {
            t.join();
        }
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << std::endl;
    }

    return 0;
}