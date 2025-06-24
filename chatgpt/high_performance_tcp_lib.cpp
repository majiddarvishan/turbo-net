// high_performance_tcp_lib.cpp
#include "high_performance_tcp_lib.hpp"
#include <boost/asio.hpp>
#include <arpa/inet.h>  // htonl, ntohl

namespace hpnet {

////////////////////////////////////////////////////////
// Connection Implementation
////////////////////////////////////////////////////////

Connection::Connection(boost::asio::ip::tcp::socket socket)
    : socket_(std::move(socket)), strand_(socket_.get_executor()) {}

Connection::~Connection() {
    close();
}

// Callback setters
void Connection::onRequest(RequestHandler h) { requestHandler_ = std::move(h); }
void Connection::onResponse(ResponseHandler h) { responseHandler_ = std::move(h); }
void Connection::onError(ErrorHandler h) { errorHandler_ = std::move(h); }

void Connection::start() {
    doReadHeader();
}

void Connection::sendPacket(const Packet& pkt) {
    auto self = shared_from_this();
    Buffer buf(sizeof(Header) + pkt.body.size());
    Header hdr = pkt.header;
    hdr.packet_length = htonl(hdr.packet_length);
    std::memcpy(buf.data(), &hdr, sizeof(Header));
    std::copy(pkt.body.begin(), pkt.body.end(), buf.begin() + sizeof(Header));

    boost::asio::post(strand_, [this, self, buf = std::move(buf)]() mutable {
        bool writing = !writeQueue_.empty();
        writeQueue_.push_back(std::move(buf));
        if (!writing) doWrite();
    });
}

void Connection::doReadHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(&readHeader_, sizeof(Header)),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
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
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (ec) { if (errorHandler_) errorHandler_(ec); return; }
            Packet pkt{readHeader_, readBody_};
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
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
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

Server::Server(boost::asio::io_context& ioc, const boost::asio::ip::tcp::endpoint& ep)
    : acceptor_(ioc, ep) {}

void Server::startAccept() { doAccept(); }
void Server::onClientConnect(ConnectHandler h) { connectHandler_ = std::move(h); }

void Server::doAccept() {
    acceptor_.async_accept([this](boost::system::error_code ec, boost::asio::ip::tcp::socket sock) {
        if (!ec) {
            auto conn = std::make_shared<Connection>(std::move(sock));
            if (connectHandler_) connectHandler_(conn);
            conn->start();
        }
        doAccept();
    });
}

////////////////////////////////////////////////////////
// Client Implementation
////////////////////////////////////////////////////////

Client::Client(boost::asio::io_context& ioc,
               const boost::asio::ip::tcp::endpoint& ep,
               Config cfg)
    : io_context_(ioc)
    , endpoint_(ep)
    , socket_(ioc)
    , reconnectTimer_(ioc)
    , watchdogTimer_(ioc)
    , config_(cfg)
    , nextSequence_(1) {}

void Client::start() { doConnect(); }
void Client::stop() {
    reconnectTimer_.cancel();
    watchdogTimer_.cancel();
    boost::system::error_code ec;
    socket_.close(ec);
}

void Client::onBindResp(BindHandler h) { bindHandler_ = std::move(h); }
void Client::onError(ErrorHandler h) { errorHandler_ = std::move(h); }

void Client::doConnect() {
    auto self = shared_from_this();
    socket_.async_connect(endpoint_, [this, self](boost::system::error_code ec) {
        if (ec) {
            if (errorHandler_) errorHandler_(ec);
            scheduleReconnect();
            return;
        }
        sendBind();
        auto conn = std::make_shared<Connection>(std::move(socket_));
        conn->onResponse([this](auto pkt){ handlePacket(pkt); });
        conn->onError(errorHandler_);
        conn->start();
        scheduleWatchdog();
    });
}

void Client::scheduleReconnect() {
    reconnectTimer_.expires_after(config_.reconnect_interval);
    reconnectTimer_.async_wait([this](boost::system::error_code ec) { if (!ec) doConnect(); });
}

void Client::scheduleTimeout(uint32_t seq) {
    auto timer = std::make_shared<boost::asio::steady_timer>(io_context_);
    timer->expires_after(config_.request_timeout);
    timer->async_wait([this, seq, timer](boost::system::error_code ec) {
        if (!ec && pendingRequests_.count(seq)) {
            pendingRequests_.erase(seq);
            if (errorHandler_) errorHandler_(boost::asio::error::timed_out);
        }
    });
}

void Client::sendRequest(const Packet& pkt, ResponseCallback cb) {
    uint32_t seq = nextSequence_++;
    Packet out = pkt;
    out.header.sequence = seq;
    out.header.packet_length = sizeof(Header) + out.body.size();
    pendingRequests_[seq] = cb;

    Buffer buf(sizeof(Header) + out.body.size());
    Header hdr = out.header; hdr.packet_length = htonl(hdr.packet_length);
    std::memcpy(buf.data(), &hdr, sizeof(Header));
    std::copy(out.body.begin(), out.body.end(), buf.begin()+sizeof(Header));

    boost::asio::async_write(socket_, boost::asio::buffer(buf), [this, seq](auto ec, auto) {
        if (ec) { if (errorHandler_) errorHandler_(ec); return; }
        scheduleTimeout(seq);
    });
}

void Client::sendBind() {
    Packet pkt;
    pkt.header.type = PacketType::BindReq;
    pkt.header.status = Status::Ok;
    std::string id = "client-id";
    pkt.body.assign(id.begin(), id.end());
    pkt.header.sequence = nextSequence_++;
    pkt.header.packet_length = sizeof(Header) + pkt.body.size();

    Buffer buf(sizeof(Header)+pkt.body.size());
    Header hdr = pkt.header; hdr.packet_length = htonl(hdr.packet_length);
    std::memcpy(buf.data(), &hdr, sizeof(Header));
    std::copy(pkt.body.begin(), pkt.body.end(), buf.begin()+sizeof(Header));
    boost::asio::async_write(socket_, boost::asio::buffer(buf), [](auto, auto){});
}

void Client::scheduleWatchdog() {
    watchdogTimer_.expires_after(config_.watchdog_interval);
    watchdogTimer_.async_wait([this](boost::system::error_code ec) { if (!ec) sendWatchdog(); });
}

void Client::sendWatchdog() {
    Packet pkt;
    pkt.header.type = PacketType::WatchdogReq;
    pkt.header.status = Status::Ok;
    pkt.header.sequence = nextSequence_++;
    pkt.header.packet_length = sizeof(Header);
    Buffer buf(sizeof(Header));
    Header hdr = pkt.header; hdr.packet_length = htonl(hdr.packet_length);
    std::memcpy(buf.data(), &hdr, sizeof(Header));
    boost::asio::async_write(socket_, boost::asio::buffer(buf), [this](auto ec, auto) { if (!ec) scheduleWatchdog(); });
}

void Client::handlePacket(const Packet& pkt) {
    if (static_cast<uint8_t>(pkt.header.type) & 0x80) {
        if (pkt.header.type == PacketType::BindResp && bindHandler_) {
            bindHandler_(pkt);
        } else {
            auto it = pendingRequests_.find(pkt.header.sequence);
            if (it != pendingRequests_.end()) {
                it->second(pkt);
                pendingRequests_.erase(it);
            }
        }
    }
}

} // namespace hpnet
