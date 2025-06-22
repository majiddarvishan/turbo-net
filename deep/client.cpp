// In client.cpp
void Client::async_request(Packet request,
                           std::function<void(Packet)> response_handler,
                           std::function<void()> timeout_handler,
                           uint32_t timeout_sec) {
    request.seq_num = next_seq_++;
    pending_requests_[request.seq_num] = {
        response_handler,
        timeout_handler,
        boost::asio::steady_timer(io_context_)
    };

    auto& timer = pending_requests_[request.seq_num].timer;
    timer.expires_after(std::chrono::seconds(timeout_sec));
    timer.async_wait([this, seq=request.seq_num](boost::system::error_code ec) {
        if (ec == boost::asio::error::operation_aborted) return;
        auto it = pending_requests_.find(seq);
        it->second.timeout_handler();
        pending_requests_.erase(it);
    });

    connection_->send(request);
}

void Client::reconnect() {
    socket_.close();
    reconnect_timer_.expires_after(std::chrono::seconds(5));
    reconnect_timer_.async_wait([this](boost::system::error_code ec) {
        if (!ec) start_connect();
    });
}