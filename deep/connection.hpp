    #pragma once
    #include <boost/asio.hpp>
    #include "packet.hpp"

    using boost::asio::ip::tcp;

    class Connection : public std::enable_shared_from_this<Connection> {
    public:
        Connection(tcp::socket socket);
        void start();
        void send(const Packet& packet);

        // Callback signatures
        using PacketCallback = std::function<void(Packet)>;
        using ErrorCallback = std::function<void()>;

        void set_packet_callback(PacketCallback cb);
        void set_error_callback(ErrorCallback cb);

    private:
        void read_header();
        void read_body(uint32_t body_length);

        tcp::socket socket_;
        std::array<uint8_t, 10> header_buf_;
        std::vector<uint8_t> body_buf_;
        PacketCallback packet_callback_;
        ErrorCallback error_callback_;
    };