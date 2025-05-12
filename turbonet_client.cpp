// File: turbonet_client.cpp
#include "turbonet_client.h"

namespace turbonet {

TurboNetClient::TurboNetClient(int readTimeoutMs,
                               int writeTimeoutMs,
                               int responseTimeoutMs,
                               std::size_t ioThreads)
    : ioCtx_(),
      workGuard_(asio::make_work_guard(ioCtx_)),
      socket_(ioCtx_),
      connectTimer_(ioCtx_),
      readTimer_(ioCtx_),
      writeTimer_(ioCtx_),
      responseSweepTimer_(ioCtx_),
      strand_(asio::make_strand(ioCtx_)),
      readTimeoutMs_(readTimeoutMs),
      writeTimeoutMs_(writeTimeoutMs),
      responseTimeoutMs_(responseTimeoutMs) {
    for (std::size_t i = 0; i < ioThreads; ++i) {
        ioThreads_.emplace_back([this]() { ioCtx_.run(); });
    }
    running_ = true;
}

TurboNetClient::~TurboNetClient() {
    close();
    workGuard_.reset();
    ioCtx_.stop();
    for (auto& t : ioThreads_) {
        if (t.joinable())
            t.join();
    }
}

void TurboNetClient::connect(const std::string& host, uint16_t port, int timeoutMs,
                             std::function<void(const asio::error_code&)> onConnect) {
    connectHandler_ = std::move(onConnect);
    asio::ip::tcp::resolver resolver(ioCtx_);
    auto endpoints = resolver.resolve(host, std::to_string(port));

    startConnectTimer(timeoutMs);
    asio::async_connect(socket_, endpoints,
        asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec, const asio::ip::tcp::endpoint&) {
            self->cancelConnectTimer();
            if (!ec) self->doReadHeader();
            if (self->connectHandler_) self->connectHandler_(ec);
        })
    );
}

void TurboNetClient::startConnectTimer(int timeoutMs) {
    if (timeoutMs <= 0) return;
    connectTimer_.expires_after(std::chrono::milliseconds(timeoutMs));
    connectTimer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec) {
        self->onConnectTimeout(ec);
    }));
}

void TurboNetClient::cancelConnectTimer() {
    try
    {
        connectTimer_.cancel();
    }
    catch(asio::error_code& ec)
    {}
}

void TurboNetClient::onConnectTimeout(const asio::error_code& ec) {
    if (!ec) {
        asio::error_code ignored;
        socket_.close(ignored);
    }
}

void TurboNetClient::doReadHeader() {
    asio::async_read(socket_, asio::buffer(headerBuf_),
        asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec, std::size_t bytes_transferred) {
            self->onReadHeader(ec, bytes_transferred);
        })
    );
}

void TurboNetClient::onReadHeader(const asio::error_code& ec, std::size_t) {
    if (ec) return;

    uint32_t packetLen = fromBigEndian(&headerBuf_[0]);
    lastPacketId_ = headerBuf_[4];
    lastStatus_ = headerBuf_[5];
    lastSequence_ = fromBigEndian(&headerBuf_[6]);

    if (packetLen < 10) return; // invalid packet
    doReadBody(packetLen - 10);
}

void TurboNetClient::doReadBody(uint32_t bodyLen) {
    bodyBuf_.resize(bodyLen);
    asio::async_read(socket_, asio::buffer(bodyBuf_),
        asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec, std::size_t bytes_transferred) {
            self->onReadBody(ec, bytes_transferred);
        })
    );
}

void TurboNetClient::onReadBody(const asio::error_code& ec, std::size_t) {
    if (!ec && onPacket_) {
        onPacket_(lastPacketId_, lastStatus_, lastSequence_, bodyBuf_);
    }
    cancelReadTimer();
    doReadHeader();
}

void TurboNetClient::sendPacket(uint8_t packetId, uint8_t status, uint32_t sequence,
                                const uint8_t* data, std::size_t len) {
    std::vector<uint8_t> packet(10 + len);
    uint32_t totalLen = 10 + static_cast<uint32_t>(len);

    uint32_t lenBE = toBigEndian(totalLen);
    std::memcpy(&packet[0], &lenBE, 4);
    packet[4] = packetId;
    packet[5] = status;
    uint32_t seqBE = toBigEndian(sequence);
    std::memcpy(&packet[6], &seqBE, 4);
    std::memcpy(&packet[10], data, len);

    asio::post(strand_, [self = shared_from_this(), packet = std::move(packet)]() mutable {
        self->txBuffer_.insert(self->txBuffer_.end(), packet.begin(), packet.end());
        if (self->txBuffer_.size() == packet.size()) {
            self->doWrite();
        }
    });
}

uint32_t TurboNetClient::sendRequest(const uint8_t* data, std::size_t len) {
    uint32_t seq = sequenceCounter_.fetch_add(1);
    sendPacket(0x02, 0x00, seq, data, len);
    startResponseTimer(seq);
    return seq;
}

void TurboNetClient::doWrite() {
    startWriteTimer();
    asio::async_write(socket_, asio::buffer(txBuffer_),
        asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec, std::size_t bytes_transferred) {
            self->onWrite(ec, bytes_transferred);
        })
    );
}

void TurboNetClient::onWrite(const asio::error_code& ec, std::size_t) {
    cancelWriteTimer();
    txBuffer_.clear();
}

void TurboNetClient::startReadTimer() {
    if (readTimeoutMs_ <= 0) return;
    readTimer_.expires_after(std::chrono::milliseconds(readTimeoutMs_));
    readTimer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec) {
        self->onReadTimeout(ec);
    }));
}

void TurboNetClient::cancelReadTimer() {
    try
    {
        readTimer_.cancel();
    }
    catch(asio::error_code& ec)
    {}
}

void TurboNetClient::onReadTimeout(const asio::error_code& ec) {
    if (!ec) socket_.close();
}

void TurboNetClient::startWriteTimer() {
    if (writeTimeoutMs_ <= 0) return;
    writeTimer_.expires_after(std::chrono::milliseconds(writeTimeoutMs_));
    writeTimer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec) {
        self->onWriteTimeout(ec);
    }));
}

void TurboNetClient::cancelWriteTimer() {
    try
    {
        writeTimer_.cancel();
    }
    catch(asio::error_code& ec)
    {}
}

void TurboNetClient::onWriteTimeout(const asio::error_code& ec) {
    if (!ec) socket_.close();
}

void TurboNetClient::startResponseTimer(uint32_t sequence) {
    if (responseTimeoutMs_ <= 0) return;
    auto expiry = std::chrono::steady_clock::now() + std::chrono::milliseconds(responseTimeoutMs_);
    {
        std::scoped_lock lock(responseMutex_);
        responseMap_[sequence] = expiry;
        responseQueue_.push({ sequence, expiry });
    }

    responseSweepTimer_.expires_after(std::chrono::milliseconds(responseTimeoutMs_));
    responseSweepTimer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec) {
        self->checkAndFireResponseTimers(ec);
    }));
}

void TurboNetClient::checkAndFireResponseTimers(const asio::error_code& ec) {
    if (ec) return;

    auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> expired;

    {
        std::scoped_lock lock(responseMutex_);
        while (!responseQueue_.empty() && responseQueue_.top().expiry <= now) {
            uint32_t seq = responseQueue_.top().sequence;
            responseQueue_.pop();
            auto it = responseMap_.find(seq);
            if (it != responseMap_.end() && it->second <= now) {
                expired.push_back(seq);
                responseMap_.erase(it);
            }
        }
    }

    for (auto seq : expired) {
        if (onTimeout_) onTimeout_(seq);
    }

    // Reset timer to next earliest
    std::chrono::steady_clock::time_point nextExpiry = now + std::chrono::hours(24);
    {
        std::scoped_lock lock(responseMutex_);
        if (!responseQueue_.empty()) {
            nextExpiry = responseQueue_.top().expiry;
        }
    }
    responseSweepTimer_.expires_at(nextExpiry);
    responseSweepTimer_.async_wait(asio::bind_executor(strand_, [self = shared_from_this()](const asio::error_code& ec) {
        self->checkAndFireResponseTimers(ec);
    }));
}

void TurboNetClient::cancelResponseTimer(uint32_t sequence) {
    std::scoped_lock lock(responseMutex_);
    responseMap_.erase(sequence);
}

void TurboNetClient::close() {
    running_ = false;
    asio::error_code ec;
    socket_.close(ec);
    cancelConnectTimer();
    cancelReadTimer();
    cancelWriteTimer();
}

void TurboNetClient::setPacketHandler(PacketHandler handler) {
    onPacket_ = std::move(handler);
}

void TurboNetClient::setTimeoutHandler(TimeoutHandler handler) {
    onTimeout_ = std::move(handler);
}

uint32_t TurboNetClient::toBigEndian(uint32_t v) {
    return htonl(v);
}

uint32_t TurboNetClient::fromBigEndian(const uint8_t* b) {
    uint32_t v;
    std::memcpy(&v, b, 4);
    return ntohl(v);
}

} // namespace turbonet
