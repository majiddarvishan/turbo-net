// In connection.cpp
void Connection::read_header() {
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(header_buf_),
        [this, self](boost::system::error_code ec, size_t) {
            if (ec) {
                if (error_callback_) error_callback_();
                return;
            }

            // Parse header
            uint32_t length = (header_buf_[0] << 24) | (header_buf_[1] << 16)
                           | (header_buf_[2] << 8) | header_buf_[3];
            uint32_t body_length = length - 10;

            read_body(body_length);
        });
}

void Connection::read_body(uint32_t body_length) {
    body_buf_.resize(body_length);
    auto self(shared_from_this());
    boost::asio::async_read(socket_, boost::asio::buffer(body_buf_),
        [this, self](boost::system::error_code ec, size_t) {
            if (ec) {
                if (error_callback_) error_callback_();
                return;
            }

            Packet packet = Packet::deserialize(header_buf_.data(), body_buf_);
            if (packet_callback_) packet_callback_(std::move(packet));
            read_header(); // Continue reading next packet
        });
}