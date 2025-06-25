#include "connection.hpp"

#include <arpa/inet.h>
#include <map>

namespace hpnet {
Connection::Connection(boost::asio::ip::tcp::socket socket)
    : socket_(std::move(socket)),
      strand_(socket_.get_executor()),
      poolIndex_(0),
      head_(0), tail_(0), count_(0) {
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
    for (size_t i = 0; i < bufferPool_.size(); ++i) {
        auto &buf = bufferPool_[poolIndex_];
        poolIndex_ = (poolIndex_ + 1) % bufferPool_.size();
        if (buf.capacity() >= sz) { buf.clear(); buf.resize(sz); return buf; }
    }
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
    // prepare header in network byte order
    Header hdr = pkt.header;
    hdr.packet_length = htonl(hdr.packet_length);
    // total packet size
    std::size_t totalSize = sizeof(Header) + pkt.body.size();
    // get a pooled buffer big enough
    Buffer &concat = acquireBuffer(totalSize);
    // copy header + body into that buffer
    std::memcpy(concat.data(), &hdr, sizeof(Header));
    std::copy(pkt.body.begin(), pkt.body.end(), concat.begin() + sizeof(Header));

    boost::asio::post(strand_, [this, self, concat=std::move(concat)]() {
        bool writing = (count_ > 0);
        enqueueWrite(concat);
        if (!writing) doWrite();
    });
}


// void Connection::sendPacket(const Packet& pkt) {
//     auto self = shared_from_this();
//     Header hdr = pkt.header;
//     hdr.packet_length = htonl(hdr.packet_length);
//     auto &bodyBuf = acquireBuffer(pkt.body.size());
//     std::copy(pkt.body.begin(), pkt.body.end(), bodyBuf.begin());

//     std::vector<boost::asio::const_buffer> bufs = {
//         boost::asio::buffer(&hdr, sizeof(Header)),
//         boost::asio::buffer(bodyBuf)
//     };

//     boost::asio::post(strand_, [this, self, bufs=std::move(bufs)]() mutable {
//         bool writing = (count_ > 0);
//         // flatten into one buffer for ring
//         Buffer concat(sizeof(Header) + boost::asio::buffer_size(bufs[1]));
//         std::memcpy(concat.data(), bufs[0].data(), sizeof(Header));
//         std::memcpy(concat.data()+sizeof(Header), bufs[1].data(), boost::asio::buffer_size(bufs[1]));
//         enqueueWrite(std::move(concat));
//         if (!writing) doWrite();
//     });
// }

void Connection::doReadHeader() {
    auto self = shared_from_this();
    boost::asio::async_read(socket_, boost::asio::buffer(&readHeader_, sizeof(Header)),
        boost::asio::bind_executor(strand_, [this, self](auto ec, auto){
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
        boost::asio::bind_executor(strand_, [this, self](auto ec, auto){
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
        boost::asio::bind_executor(strand_, [this, self](auto ec, auto){
            if (ec) return errorHandler_ ? errorHandler_(ec) : void();
            if (count_ > 0) doWrite();
        }));
}

void Connection::sendResponse(const Packet& request, const Buffer& body, Status status) {
    Packet resp;
    resp.header.sequence = request.header.sequence;
    resp.header.type = static_cast<PacketType>(static_cast<uint8_t>(request.header.type) | 0x80);
    resp.header.status = status;
    resp.body = body;
    resp.header.packet_length = sizeof(Header) + resp.body.size();
    sendPacket(resp);
}

void Connection::close() {
    boost::system::error_code ec;
    socket_.close(ec);
}
} // namespace hpnet
