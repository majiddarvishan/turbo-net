#include "turbonet_client.h"
#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

namespace turbonet {

static uint32_t toBigEndian(uint32_t v) {
    return ((v & 0xFF) << 24) |
           ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) |
           ((v & 0xFF000000) >> 24);
}

static uint32_t fromBigEndian(const uint8_t* b) {
    return (uint32_t(b[0]) << 24) |
           (uint32_t(b[1]) << 16) |
           (uint32_t(b[2]) << 8)  |
           (uint32_t(b[3]));
}

TurboNetClient::TurboNetClient(int readTimeoutMs, int writeTimeoutMs, std::size_t ioThreads)
  : socket_(ioCtx_)
  , connectTimer_(ioCtx_)
  , readTimer_(ioCtx_)
  , writeTimer_(ioCtx_)
  , strand_(ioCtx_.get_executor())
  , readTimeoutMs_(readTimeoutMs)
  , writeTimeoutMs_(writeTimeoutMs)
{
    for (std::size_t i = 0; i < ioThreads; ++i) ioThreads_.emplace_back([this]{ ioCtx_.run(); });
}

TurboNetClient::~TurboNetClient() { close(); }

void TurboNetClient::connect(const std::string& host, uint16_t port, int timeoutMs,
                              std::function<void(const asio::error_code&)> onConnect) {
    if (running_) return;
    running_ = true;
    connectHandler_ = onConnect;

    asio::ip::tcp::resolver resolver(ioCtx_);
    auto endpoints = resolver.resolve(host, std::to_string(port));
    auto self = shared_from_this();

    startConnectTimer(timeoutMs);
    asio::async_connect(socket_, endpoints,
        strand_.wrap([this, self](const asio::error_code& ec, auto){
            cancelConnectTimer();
            if (!ec) doReadHeader();
            running_ = !ec;
            connectHandler_(ec);
        }));
}

uint32_t TurboNetClient::sendRequest(const uint8_t* data, std::size_t len) {
    uint32_t seq = sequenceCounter_.fetch_add(1, std::memory_order_relaxed);
    sendPacket(0x02, 0x00, seq, data, len);
    if (responseTimeoutMs_ > 0 && onTimeout_) {
        startResponseTimer(seq);
    }
    return seq;
}

void TurboNetClient::startResponseTimer(uint32_t sequence) {
    std::lock_guard<std::mutex> lock(responseMutex_);
    auto timer = std::make_unique<asio::steady_timer>(ioCtx_);
    timer->expires_after(std::chrono::milliseconds(responseTimeoutMs_));
    auto self = shared_from_this();
    timer->async_wait(strand_.wrap([this, self, sequence](const asio::error_code& ec) {
        onResponseTimeout(sequence, ec);
    }));
    responseTimers_[sequence] = std::move(timer);
}

void TurboNetClient::cancelResponseTimer(uint32_t sequence) {
    std::lock_guard<std::mutex> lock(responseMutex_);
    auto it = responseTimers_.find(sequence);
    if (it != responseTimers_.end()) {
        asio::error_code ec;
        it->second->cancel(ec);
        responseTimers_.erase(it);
    }
}

void TurboNetClient::onResponseTimeout(uint32_t sequence, const asio::error_code& ec) {
    if (ec != asio::error::operation_aborted) {
        // Timeout fired
        if (onTimeout_) onTimeout_(sequence);
        std::lock_guard<std::mutex> lock(responseMutex_);
        responseTimers_.erase(sequence);
    }
}

void TurboNetClient::onReadBody(const asio::error_code& ec, std::size_t) {
    if (ec) { close(); return; }
    // Cancel response timer if this was a request-response
    cancelResponseTimer(lastSequence_);
    if (onPacket_) onPacket_(lastPacketId_, lastStatus_, lastSequence_, bodyBuf_);
    doReadHeader();
}

void TurboNetClient::sendPacket(uint8_t packetId, uint8_t status, uint32_t sequence,
                                const uint8_t* data, std::size_t len) {
    auto self = shared_from_this();
    asio::post(strand_, [this, self, packetId, status, sequence, data, len]{
        uint32_t totalLen = uint32_t(10 + len);
        uint32_t beTotal = toBigEndian(totalLen);
        // build header
        uint8_t hdr[10];
        std::memcpy(hdr, &beTotal, 4);
        hdr[4] = packetId;
        hdr[5] = status;
        uint32_t beSeq = toBigEndian(sequence);
        std::memcpy(hdr+6, &beSeq, 4);

        txBuffer_.insert(txBuffer_.end(), hdr, hdr+10);
        txBuffer_.insert(txBuffer_.end(), data, data+len);
        if (txBuffer_.size() == 10 + len) doWrite();
    });
}

void TurboNetClient::setPacketHandler(PacketHandler handler) { onPacket_ = std::move(handler); }

void TurboNetClient::close() {
    if (!running_) return;
    running_ = false;
    asio::post(ioCtx_, [this]{ socket_.close(); });
    ioCtx_.stop();
    for (auto& t : ioThreads_) if (t.joinable()) t.join();
}

void TurboNetClient::startReadTimer() {
    if (readTimeoutMs_ <= 0) return;
    readTimer_.expires_after(std::chrono::milliseconds(readTimeoutMs_));
    auto self = shared_from_this();
    readTimer_.async_wait(strand_.wrap([this, self](const asio::error_code& ec){ onReadTimeout(ec); }));
}

void TurboNetClient::cancelReadTimer() { asio::error_code ec; readTimer_.cancel(ec); }

void TurboNetClient::onReadTimeout(const asio::error_code& ec) {
    if (ec != asio::error::operation_aborted) { socket_.close(); running_ = false; }
}

void TurboNetClient::doReadHeader() {
    startReadTimer();
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(headerBuf_),
        strand_.wrap([this, self](const asio::error_code& ec, std::size_t){
            cancelReadTimer(); onReadHeader(ec, 10);
        }));
}

void TurboNetClient::onReadHeader(const asio::error_code& ec, std::size_t) {
    if (ec) { close(); return; }
    uint32_t totalLen = fromBigEndian(headerBuf_.data());
    uint8_t packetId = headerBuf_[4];
    uint8_t status   = headerBuf_[5];
    uint32_t sequence = fromBigEndian(headerBuf_.data()+6);
    uint32_t bodyLen = totalLen - 10;
    // store header fields in bodyBuf_ front space for callback convenience
    bodyBuf_.resize(bodyLen);
    doReadBody(bodyLen);
    // Save header fields in lambda capture for onReadBody
    lastPacketId_ = packetId;
    lastStatus_   = status;
    lastSequence_ = sequence;
}

void TurboNetClient::doReadBody(uint32_t bodyLen) {
    startReadTimer();
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(bodyBuf_),
        strand_.wrap([this, self](const asio::error_code& ec, std::size_t bytes){
            cancelReadTimer(); onReadBody(ec, bytes);
        }));
}

void TurboNetClient::onReadBody(const asio::error_code& ec, std::size_t) {
    if (ec) { close(); return; }
    if (onPacket_) onPacket_(lastPacketId_, lastStatus_, lastSequence_, bodyBuf_);
    doReadHeader();
}

void TurboNetClient::startWriteTimer() {
    if (writeTimeoutMs_ <= 0) return;
    writeTimer_.expires_after(std::chrono::milliseconds(writeTimeoutMs_));
    auto self = shared_from_this();
    writeTimer_.async_wait(strand_.wrap([this, self](const asio::error_code& ec){ onWriteTimeout(ec); }));
}

void TurboNetClient::cancelWriteTimer() { asio::error_code ec; writeTimer_.cancel(ec); }

void TurboNetClient::onWriteTimeout(const asio::error_code& ec) {
    if (ec != asio::error::operation_aborted) { socket_.close(); running_ = false; }
}

void TurboNetClient::doWrite() {
    startWriteTimer();
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(txBuffer_),
        strand_.wrap([this, self](const asio::error_code& ec, std::size_t bytes){
            cancelWriteTimer(); onWrite(ec, bytes);
        }));
}

void TurboNetClient::onWrite(const asio::error_code& ec, std::size_t bytes_transferred) {
    if (ec) { close(); return; }
    txBuffer_.erase(txBuffer_.begin(), txBuffer_.begin() + bytes_transferred);
    if (!txBuffer_.empty()) doWrite();
}

} // namespace turbonet
