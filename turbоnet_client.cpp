#include "turbоnet_client.h"

#include <asio/connect.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/write.hpp>

namespace turbоnet {

static uint32_t hostToBigEndian(uint32_t v) {
    return ((v & 0xFF) << 24) |
           ((v & 0xFF00) << 8) |
           ((v & 0xFF0000) >> 8) |
           ((v & 0xFF000000) >> 24);
}

static uint32_t bigEndianToHost(const std::array<uint8_t,4>& b) {
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
    // Launch IO thread pool
    for (std::size_t i = 0; i < ioThreads; ++i) {
        ioThreads_.emplace_back([this]{ ioCtx_.run(); });
    }
}

TurboNetClient::~TurboNetClient() {
    close();
}

void TurboNetClient::connect(
    const std::string& host,
    uint16_t port,
    int timeoutMs,
    std::function<void(const asio::error_code&)> onConnect)
{
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

void TurboNetClient::startConnectTimer(int timeoutMs) {
    connectTimer_.expires_after(std::chrono::milliseconds(timeoutMs));
    auto self = shared_from_this();
    connectTimer_.async_wait(strand_.wrap([this, self](const asio::error_code& ec){
        onConnectTimeout(ec);
    }));
}

void TurboNetClient::cancelConnectTimer() {
    asio::error_code ec; connectTimer_.cancel(ec);
}

void TurboNetClient::onConnectTimeout(const asio::error_code& ec) {
    if (ec != asio::error::operation_aborted) {
        socket_.close(); running_ = false;
        connectHandler_(asio::error::make_error_code(asio::error::timed_out));
    }
}

void TurboNetClient::send(const uint8_t* data, std::size_t len) {
    auto self = shared_from_this();
    asio::post(strand_, [this, self, dataPtr=data, len]{
        uint32_t beLen = hostToBigEndian((uint32_t)len);
        txBuffer_.insert(txBuffer_.end(), reinterpret_cast<uint8_t*>(&beLen), reinterpret_cast<uint8_t*>(&beLen)+4);
        txBuffer_.insert(txBuffer_.end(), dataPtr, dataPtr+len);
        if (txBuffer_.size() == len + 4) {
            doWrite();
        }
    });
}

void TurboNetClient::setDataHandler(DataHandler handler) {
    onData_ = std::move(handler);
}

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
    readTimer_.async_wait(strand_.wrap([this, self](const asio::error_code& ec){
        onReadTimeout(ec);
    }));
}

void TurboNetClient::cancelReadTimer() {
    asio::error_code ec; readTimer_.cancel(ec);
}

void TurboNetClient::onReadTimeout(const asio::error_code& ec) {
    if (ec != asio::error::operation_aborted) {
        socket_.close(); running_ = false;
    }
}

void TurboNetClient::doReadHeader() {
    startReadTimer();
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(headerBuf_),
        strand_.wrap([this, self](const asio::error_code& ec, std::size_t bytes){
            cancelReadTimer(); onReadHeader(ec, bytes);
        }));
}

void TurboNetClient::onReadHeader(const asio::error_code& ec, std::size_t) {
    if (ec) { close(); return; }
    uint32_t bodyLen = bigEndianToHost(headerBuf_);
    doReadBody(bodyLen);
}

void TurboNetClient::doReadBody(std::uint32_t bodyLen) {
    bodyBuf_.assign(bodyLen, 0);
    startReadTimer();
    auto self = shared_from_this();
    asio::async_read(socket_, asio::buffer(bodyBuf_),
        strand_.wrap([this, self](const asio::error_code& ec, std::size_t bytes){
            cancelReadTimer(); onReadBody(ec, bytes);
        }));
}

void TurboNetClient::onReadBody(const asio::error_code& ec, std::size_t) {
    if (ec) { close(); return; }
    if (onData_) onData_(bodyBuf_);
    doReadHeader();
}

void TurboNetClient::startWriteTimer() {
    if (writeTimeoutMs_ <= 0) return;
    writeTimer_.expires_after(std::chrono::milliseconds(writeTimeoutMs_));
    auto self = shared_from_this();
    writeTimer_.async_wait(strand_.wrap([this, self](const asio::error_code& ec){
        onWriteTimeout(ec);
    }));
}

void TurboNetClient::cancelWriteTimer() {
    asio::error_code ec; writeTimer_.cancel(ec);
}

void TurboNetClient::onWriteTimeout(const asio::error_code& ec) {
    if (ec != asio::error::operation_aborted) {
        socket_.close(); running_ = false;
    }
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

} // namespace turbоnet
