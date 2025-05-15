// File: turbonet_server.cpp
#include "turbonet_server.h"
#include <asio/read.hpp>
#include <asio/write.hpp>
#include <cstring>

namespace turbonet {

// ----- Session Implementation -----
TurboNetServer::Session::Session(asio::ip::tcp::socket sock,
                                 TurboNetServer& server)
    : socket(std::move(sock)),
      strand_(socket.get_executor()),   // executor_type matches socket
      serverRef(server)
{}

void TurboNetServer::Session::start() {
    doReadHeader();
}

void TurboNetServer::Session::doReadHeader() {
    auto self = shared_from_this();
    asio::async_read(socket,
        asio::buffer(headerBuf),
        asio::bind_executor(strand_,
            [this, self](auto ec, auto len){ onReadHeader(ec,len); }));
}

void TurboNetServer::Session::onReadHeader(const asio::error_code& ec, std::size_t) {
    if (ec) { serverRef.removeSession(shared_from_this()); return; }
    uint32_t total = ntohl(*reinterpret_cast<uint32_t*>(headerBuf.data()));
    uint8_t pid = headerBuf[4];
    uint8_t status = headerBuf[5];
    uint32_t seq = ntohl(*reinterpret_cast<uint32_t*>(headerBuf.data()+6));
    uint32_t bodyLen = total - 10;
    doReadBody(bodyLen);
}

void TurboNetServer::Session::doReadBody(uint32_t len) {
    bodyBuf.resize(len);
    auto self = shared_from_this();
    asio::async_read(socket,
        asio::buffer(bodyBuf),
        asio::bind_executor(strand_,
            [this, self](auto ec, auto l){ onReadBody(ec,l); }));
}

void TurboNetServer::Session::onReadBody(const asio::error_code& ec, std::size_t) {
    if (ec) { serverRef.removeSession(shared_from_this()); return; }

    // Handle bind request
    if (!authenticated && headerBuf[4] == 0x01) {
        std::string cid(bodyBuf.begin(), bodyBuf.end());
        if (serverRef.onAuth_ && serverRef.onAuth_(cid)) {
            authenticated = true;
            // Send bind-resp
            std::vector<uint8_t> resp(10);
            uint32_t be = htonl(10);
            std::memcpy(resp.data(), &be, 4);
            resp[4] = 0x81; resp[5] = 0x00; // success status
            uint32_t seq = ntohl(*reinterpret_cast<uint32_t*>(headerBuf.data()+6));
            uint32_t beSeq = htonl(seq);
            std::memcpy(resp.data()+6, &beSeq, 4);
            send(resp);
        } else {
            serverRef.removeSession(shared_from_this());
        }
    }
    else if (authenticated) {
        // Forward to application handler
        auto respond = [this, self=shared_from_this()](uint8_t rpId, uint8_t rpStatus, uint32_t rpSeq, const std::vector<uint8_t>& rpBody){
            uint32_t tot = static_cast<uint32_t>(10 + rpBody.size());
            std::vector<uint8_t> msg(10 + rpBody.size());
            uint32_t be = htonl(tot);
            std::memcpy(msg.data(), &be, 4);
            msg[4]=rpId; msg[5]=rpStatus;
            uint32_t seqbe = htonl(rpSeq);
            std::memcpy(msg.data()+6, &seqbe, 4);
            std::memcpy(msg.data()+10, rpBody.data(), rpBody.size());
            send(msg);
        };
        if (serverRef.onRequest_) {
            serverRef.onRequest_(headerBuf[4], headerBuf[5], ntohl(*reinterpret_cast<uint32_t*>(headerBuf.data()+6)), bodyBuf, respond);
        }
    }

    // Continue reading
    doReadHeader();
}

void TurboNetServer::Session::send(const std::vector<uint8_t>& message) {
    auto self = shared_from_this();
    asio::async_write(socket,
        asio::buffer(message),
        asio::bind_executor(strand_, [self](const asio::error_code&, std::size_t){ }));
}

// ----- Server Implementation -----
TurboNetServer::TurboNetServer(uint16_t port,
                               std::size_t maxConn,
                               std::size_t ioThreads)
    : ioCtx_(),
      workGuard_(asio::make_work_guard(ioCtx_)),
      acceptor_(ioCtx_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
      maxConnections_(maxConn)
{
    for (std::size_t i = 0; i < ioThreads; ++i)
        ioThreads_.emplace_back([this]{ ioCtx_.run(); });
}

TurboNetServer::~TurboNetServer() {
    stop();
    workGuard_.reset();
    ioCtx_.stop();
    for (auto& t : ioThreads_)
        if (t.joinable()) t.join();
}

void TurboNetServer::setAuthHandler(AuthHandler auth) {
    onAuth_ = std::move(auth);
}

void TurboNetServer::setMaxConnections(std::size_t maxConn) {
    maxConnections_ = maxConn;
}

void TurboNetServer::start(PacketHandler handler) {
    onRequest_ = std::move(handler);
    doAccept();
}

void TurboNetServer::stop() {
    asio::error_code ec;
    acceptor_.close(ec);
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    for (auto& s : sessions_) {
        s->socket.close(ec);
    }
    sessions_.clear();
}

void TurboNetServer::doAccept() {
    auto socket = std::make_shared<asio::ip::tcp::socket>(ioCtx_);
    acceptor_.async_accept(*socket, [this, socket](const asio::error_code& ec){
        if (!ec) {
            std::lock_guard<std::mutex> lock(sessionsMutex_);
            if (sessions_.size() < maxConnections_) {
                auto sess = std::make_shared<Session>(std::move(*socket), *this);
                sessions_.insert(sess);
                sess->start();
            }
        }
        doAccept();
    });
}

void TurboNetServer::removeSession(std::shared_ptr<Session> sess) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);
    sessions_.erase(sess);
}

} // namespace turbonet
