#include "high_performance_tcp_lib.hpp"

namespace hpnet {

////////////////////////////////////////////////////////
// Connection Implementation
////////////////////////////////////////////////////////

void Connection::start() {
    doReadHeader();
}

void Connection::sendPacket(const Packet& pkt) {
    auto self = shared_from_this();
    // serialize header + body
    Buffer buf(sizeof(Header) + pkt.body.size());
    std::memcpy(buf.data(), &pkt.header, sizeof(Header));
    std::copy(pkt.body.begin(), pkt.body.end(), buf.begin() + sizeof(Header));

    boost::asio::post(strand_, [this, self, buf = std::move(buf)]() mutable {
        bool writeInProgress = !writeQueue_.empty();
        writeQueue_.push_back(std::move(buf));
        if (!writeInProgress) doWrite();
    });
}

void Connection::doReadHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(&readHeader_, sizeof(Header)),
        boost::asio::bind_executor(strand_, [this, self](auto ec, auto) {
            if (ec) { if (errorHandler_) errorHandler_(ec); return; }
            readHeader_.packet_length = ntohl(readHeader_.packet_length);
            if (readHeader_.packet_length <= sizeof(Header)) {
                if (errorHandler_) errorHandler_(boost::asio::error::invalid_argument);
                return;
            }
            readBody_.resize(readHeader_.packet_length - sizeof(Header));
            doReadBody();
        }));
}

void Connection::doReadBody() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(readBody_),
        boost::asio::bind_executor(strand_, [this, self](auto ec, auto) {
            if (ec) { if (errorHandler_) errorHandler_(ec); return; }
            Packet pkt{readHeader_, readBody_};
            // route packet
            if (static_cast<uint8_t>(pkt.header.type) & 0x80) {
                if (responseHandler_) responseHandler_(pkt);
            } else {
                if (requestHandler_) requestHandler_(pkt, self);
            }
            doReadHeader();
        }));
}

void Connection::doWrite() {
    auto self = shared_from_this();
    boost::asio::async_write(socket_, boost::asio::buffer(writeQueue_.front()),
        boost::asio::bind_executor(strand_, [this, self](auto ec, auto) {
            if (ec) { if (errorHandler_) errorHandler_(ec); return; }
            writeQueue_.pop_front();
            if (!writeQueue_.empty()) doWrite();
        }));
}

void Connection::close() {
    boost::system::error_code ec;
    socket_.close(ec);
}

////////////////////////////////////////////////////////
// Server Implementation
////////////////////////////////////////////////////////

void Server::doAccept() {
    acceptor_.async_accept([this](auto ec, auto socket) {
        if (!ec) {
            auto conn = std::make_shared<Connection>(std::move(socket));
            if (connectHandler_) connectHandler_(conn);
            conn->start();
        }
        doAccept();
    });
}

////////////////////////////////////////////////////////
// Client Implementation
////////////////////////////////////////////////////////

void Client::start() {
    doConnect();
}

void Client::stop() {
    reconnectTimer_.cancel();
    watchdogTimer_.cancel();
    boost::system::error_code ec;
    socket_.close(ec);
}

void Client::doConnect() {
    auto self = shared_from_this();
    socket_.async_connect(endpoint_, [this, self](auto ec) {
        if (ec) {
            if (errorHandler_) errorHandler_(ec);
            scheduleReconnect();
            return;
        }
        // upon connect, send bind
        sendBind();
        // start reading
        auto conn = std::make_shared<Connection>(std::move(socket_));
        conn->onResponse([this](auto pkt){ handleRead(pkt); });
        conn->onError(errorHandler_);
        conn->start();
        scheduleWatchdog();
    });
}

void Client::scheduleReconnect() {
    reconnectTimer_.expires_after(config_.reconnect_interval);
    reconnectTimer_.async_wait([this](auto ec) {
        if (!ec) doConnect();
    });
}

void Client::sendRequest(Packet pkt, ResponseCallback cb) {
    pkt.header.sequence = nextSequence_++;
    pendingRequests_[pkt.header.sequence] = cb;
    pkt.header.packet_length = htonl(sizeof(Header) + pkt.body.size());
    // send via Connection wrapper
    // reuse underlying socket
    auto buf = std::make_shared<Buffer>(sizeof(Header) + pkt.body.size());
    std::memcpy(buf->data(), &pkt.header, sizeof(Header));
    std::copy(pkt.body.begin(), pkt.body.end(), buf->begin()+sizeof(Header));
    boost::asio::async_write(socket_, boost::asio::buffer(*buf), [this, buf, seq=pkt.header.sequence](auto ec, auto){
        if (ec) { if (errorHandler_) errorHandler_(ec); return; }
        scheduleTimeout(seq);
    });
}

void Client::scheduleTimeout(uint32_t seq) {
    auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
    timer->expires_after(config_.request_timeout);
    timer->async_wait([this, seq, timer](auto ec) {
        if (!ec && pendingRequests_.count(seq)) {
            pendingRequests_.erase(seq);
            // timeout callback
            if (errorHandler_) errorHandler_(boost::asio::error::timed_out);
        }
    });
}

void Client::sendBind() {
    Packet pkt;
    pkt.header.type = PacketType::BindReq;
    pkt.header.status = Status::Ok;
    std::string id = "client-id";
    pkt.body.assign(id.begin(), id.end());
    pkt.header.sequence = nextSequence_++;
    pkt.header.packet_length = htonl(sizeof(Header) + pkt.body.size());
    boost::asio::async_write(socket_, boost::asio::buffer(pkt.body), [](auto, auto){});
}

void Client::scheduleWatchdog() {
    watchdogTimer_.expires_after(config_.watchdog_interval);
    watchdogTimer_.async_wait([this](auto ec) {
        if (!ec) sendWatchdog();
    });
}

void Client::sendWatchdog() {
    Packet pkt;
    pkt.header.type = PacketType::WatchdogReq;
    pkt.header.status = Status::Ok;
    pkt.body.clear();
    pkt.header.sequence = nextSequence_++;
    pkt.header.packet_length = htonl(sizeof(Header));
    boost::asio::async_write(socket_, boost::asio::buffer(&pkt.header, sizeof(Header)), [this](auto ec, auto){
        if (!ec) scheduleWatchdog();
    });
}

void Client::handleRead(const Packet& pkt) {
    // match responses
    if (static_cast<uint8_t>(pkt.header.type) & 0x80) {
        auto seq = pkt.header.sequence;
        auto it = pendingRequests_.find(seq);
        if (it != pendingRequests_.end()) {
            it->second(pkt);
            pendingRequests_.erase(it);
        } else if (pkt.header.type == PacketType::BindResp && bindHandler_) {
            bindHandler_(pkt);
        }
    }
}

} // namespace hpnet
