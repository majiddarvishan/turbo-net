// high_performance_tcp_lib.cpp
#include "high_performance_tcp_lib.hpp"
#include <boost/asio.hpp>
#include <arpa/inet.h>  // htonl, ntohl

namespace hpnet {

////////////////////////////////////////////////////////
// Connection Implementation with optimizations
////////////////////////////////////////////////////////

Connection::Connection(boost::asio::ip::tcp::socket socket)
    : socket_(std::move(socket)),
      strand_(socket_.get_executor()),
      poolIndex_(0), head_(0), tail_(0), count_(0) {
    ring_.resize(1024);
    bufferPool_.resize(1024);
}

Connection::~Connection() {
    close();
}

void Connection::onRequest(RequestHandler h)  { requestHandler_ = std::move(h); }
void Connection::onResponse(ResponseHandler h) { responseHandler_ = std::move(h); }
void Connection::onError(ErrorHandler h)       { errorHandler_ = std::move(h); }

void Connection::start() {
    doReadHeader();
}

Buffer& Connection::acquireBuffer(std::size_t sz) {
    // simple round-robin pool
    for (size_t i = 0; i < bufferPool_.size(); ++i) {
        auto &buf = bufferPool_[poolIndex_];
        poolIndex_ = (poolIndex_ + 1) % bufferPool_.size();
        if (buf.capacity() >= sz) {
            buf.clear(); buf.resize(sz);
            return buf;
        }
    }
    // expand pool if none fits
    bufferPool_.emplace_back(sz);
    return bufferPool_.back();
}

void Connection::enqueueWrite(Buffer buf) {
    if (count_ == ring_.size()) ring_.resize(ring_.size()*2);
    ring_[tail_] = std::move(buf);
    tail_ = (tail_ + 1) % ring_.size();
    ++count_;
}

Buffer Connection::dequeueWrite() {
    Buffer buf = std::move(ring_[head_]);
    head_ = (head_ + 1) % ring_.size();
    --count_;
    return buf;
}

void Connection::sendPacket(const Packet& pkt) {
    auto self = shared_from_this();
    // prepare header buffer
    Header hdr = pkt.header;
    hdr.packet_length = htonl(hdr.packet_length);
    // acquire body buffer
    auto &bodyBuf = acquireBuffer(pkt.body.size());
    std::copy(pkt.body.begin(), pkt.body.end(), bodyBuf.begin());

    // scatter-gather buffers
    std::array<boost::asio::const_buffer, 2> bufs = {
        boost::asio::buffer(&hdr, sizeof(Header)),
        boost::asio::buffer(bodyBuf)
    };

    boost::asio::post(strand_, [this, self, bufs]() mutable {
        bool writing = (count_ > 0);
        // serialize into single buffer for ring (optional pooling)
        Buffer concat;
        concat.resize(sizeof(Header) + boost::asio::buffer_size(bufs[1]));
        std::memcpy(concat.data(), bufs[0].data(), sizeof(Header));
        std::memcpy(concat.data()+sizeof(Header), bufs[1].data(), boost::asio::buffer_size(bufs[1]));
        enqueueWrite(std::move(concat));
        if (!writing) doWrite();
    });
}

void Connection::doReadHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_,
        boost::asio::buffer(&readHeader_, sizeof(Header)),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (ec) return errorHandler_ ? errorHandler_(ec) : void();
            readHeader_.packet_length = ntohl(readHeader_.packet_length);
            if (readHeader_.packet_length <= sizeof(Header))
                return errorHandler_ ? errorHandler_(boost::asio::error::invalid_argument) : void();
            readBody_.resize(readHeader_.packet_length - sizeof(Header));
            doReadBody();
        }));
}

void Connection::doReadBody() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(readBody_),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (ec) return errorHandler_ ? errorHandler_(ec) : void();
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
    if (count_ == 0) return;
    auto self = shared_from_this();
    Buffer buf = dequeueWrite();
    boost::asio::async_write(socket_, boost::asio::buffer(buf),
        boost::asio::bind_executor(strand_, [this, self](boost::system::error_code ec, std::size_t) {
            if (ec) return errorHandler_ ? errorHandler_(ec) : void();
            if (count_ > 0) doWrite();
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
    : socket_(ioc), io_context_(ioc), endpoint_(ep),
      reconnectTimer_(ioc), watchdogTimer_(ioc),
      config_(cfg), nextSequence_(1) {}

Client::~Client() { stop(); }

void Client::start() { doConnect(); }

void Client::stop() {
    reconnectTimer_.cancel();
    watchdogTimer_.cancel();
    boost::system::error_code ec;
    socket_.close(ec);
}

void Client::onBindResp(BindHandler h) { bindHandler_ = std::move(h); }
void Client::onError(ErrorHandler h)    { errorHandler_ = std::move(h); }

void Client::doConnect() {
    auto self = shared_from_this();
    socket_.async_connect(endpoint_, [this, self](boost::system::error_code ec) {
        if (ec) {
            if (errorHandler_) errorHandler_(ec);
            scheduleReconnect();
            return;
        }
        // after connect
        sendBind();
        // reuse Connection for reading
        auto conn = std::make_shared<Connection>(std::move(socket_));
        conn->onResponse([this](auto pkt){ handlePacket(pkt); });
        conn->onError(errorHandler_);
        conn->start();
        scheduleWatchdog();
    });
}

void Client::scheduleReconnect() {
    reconnectTimer_.expires_after(config_.reconnect_interval);
    reconnectTimer_.async_wait([this](boost::system::error_code ec) {
        if (!ec) doConnect();
    });
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

    // serialize header and body
    std::vector<uint8_t> buf(sizeof(Header) + out.body.size());
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
    sendRequest(pkt, [&](const Packet& resp){ if (bindHandler_) bindHandler_(resp); });
}

void Client::scheduleWatchdog() {
    watchdogTimer_.expires_after(config_.watchdog_interval);
    watchdogTimer_.async_wait([this](boost::system::error_code ec) {
        if (!ec) {
            Packet pkt;
            pkt.header.type = PacketType::WatchdogReq;
            pkt.header.status = Status::Ok;
            pkt.header.sequence = nextSequence_++;
            pkt.header.packet_length = sizeof(Header);
            sendRequest(pkt, [&](auto){ /* ignore resp */ });
            scheduleWatchdog();
        }
    });
}

void Client::handlePacket(const Packet& pkt) {
    if (pkt.header.type == PacketType::BindResp) {
        if (bindHandler_) bindHandler_(pkt);
    } else {
        auto it = pendingRequests_.find(pkt.header.sequence);
        if (it != pendingRequests_.end()) {
            it->second(pkt);
            pendingRequests_.erase(it);
        }
    }
}

} // namespace hpnet
