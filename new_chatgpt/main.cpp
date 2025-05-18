// SMPP High-Performance C++ Library with Shared Session and Dedicated Client Class
// Uses Boost.Asio for async networking, fragmentation, timers, and callbacks

#include <boost/asio.hpp>
#include <boost/endian/conversion.hpp>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <functional>

using boost::asio::ip::tcp;
using asio_timer = boost::asio::steady_timer;

// SMPP PDU base
class SmppPdu {
public:
    uint32_t command_length;
    uint32_t command_id;
    uint32_t command_status;
    uint32_t sequence_number;
    std::vector<uint8_t> body;

    virtual ~SmppPdu() = default;

    std::vector<uint8_t> serialize() const {
        uint32_t len = 16 + body.size();
        std::vector<uint8_t> buf(len);
        uint32_t *hdr = reinterpret_cast<uint32_t*>(buf.data());
        hdr[0] = boost::endian::native_to_big(len);
        hdr[1] = boost::endian::native_to_big(command_id);
        hdr[2] = boost::endian::native_to_big(command_status);
        hdr[3] = boost::endian::native_to_big(sequence_number);
        if (!body.empty()) std::copy(body.begin(), body.end(), buf.begin() + 16);
        return buf;
    }

    static std::shared_ptr<SmppPdu> deserialize(const uint8_t* data, size_t size) {
        if (size < 16) return nullptr;
        uint32_t cmd_len = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data));
        if (size < cmd_len) return nullptr;
        auto pdu = std::make_shared<SmppPdu>();
        pdu->command_length = cmd_len;
        pdu->command_id = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data+4));
        pdu->command_status = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data+8));
        pdu->sequence_number = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data+12));
        pdu->body.assign(data+16, data+cmd_len);
        return pdu;
    }
};

// Enquire Link PDU
class EnquireLink : public SmppPdu {
public:
    EnquireLink(uint32_t seq) { command_id = 0x00000015; command_status = 0; sequence_number = seq; }
};

// Shared Session: handles client or server connection
class SmppSession : public std::enable_shared_from_this<SmppSession> {
    tcp::socket socket_;
    std::vector<uint8_t> recv_buffer_;
    asio_timer inactivity_timer_;
    std::unordered_map<uint32_t, asio_timer> pending_timers_;
    boost::asio::io_context& io_context_;
    std::atomic<uint32_t> seq_{1};

public:
    // Callbacks
    std::function<void(std::shared_ptr<SmppPdu>)> on_request;
    std::function<void(std::shared_ptr<SmppPdu>)> on_response;
    std::function<void(uint32_t)> on_timeout;
    std::function<void()> on_close;

    explicit SmppSession(boost::asio::io_context& ctx)
      : socket_(ctx), inactivity_timer_(ctx), io_context_(ctx) {}

    tcp::socket& socket() { return socket_; }

    void start() { reset_inactivity_timer(); do_read(); schedule_enquire_link(); }
    void close() { boost::system::error_code ec; socket_.close(ec); if (on_close) on_close(); }

    void send_pdu(std::shared_ptr<SmppPdu> pdu, std::chrono::seconds timeout = std::chrono::seconds(30)) {
        uint32_t seq = pdu->sequence_number = seq_++;
        auto buf = pdu->serialize();
        auto self = shared_from_this();
        asio_timer timer(io_context_, timeout);
        pending_timers_.emplace(seq, std::move(timer));
        auto &t = pending_timers_[seq];
        t.async_wait([this, self, seq](auto ec) { if (!ec) { pending_timers_.erase(seq); if (on_timeout) on_timeout(seq); }});
        boost::asio::async_write(socket_, boost::asio::buffer(buf), [this, self](auto ec, auto){ if (ec) close(); });
    }

    // For client: connect then start
    void connect(const std::string& host, unsigned short port) {
        socket_.connect({boost::asio::ip::address::from_string(host), port});
        start();
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(std::back_inserter(recv_buffer_), 4096),
            [this, self](auto ec, auto){ if (ec) { close(); return; } reset_inactivity_timer(); parse_buffer(); do_read(); });
    }

    void parse_buffer() {
        while (recv_buffer_.size() >= 16) {
            uint32_t cmd_len = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(recv_buffer_.data()));
            if (recv_buffer_.size() < cmd_len) break;
            auto pdu = SmppPdu::deserialize(recv_buffer_.data(), cmd_len);
            recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin() + cmd_len);
            if (!pdu) continue;
            auto it = pending_timers_.find(pdu->sequence_number);
            if (it != pending_timers_.end()) { it->second.cancel(); pending_timers_.erase(it); if (on_response) on_response(pdu); }
            else if (on_request) on_request(pdu);
        }
    }

    void reset_inactivity_timer() { inactivity_timer_.expires_after(std::chrono::seconds(60)); inactivity_timer_.async_wait([this](auto ec){ if (!ec) close(); }); }
    void schedule_enquire_link() { auto self=shared_from_this(); auto timer = std::make_shared<asio_timer>(io_context_, std::chrono::seconds(30));; timer->async_wait([this,self,timer](auto ec){ if (!ec) { send_pdu(std::make_shared<EnquireLink>(seq_++)); schedule_enquire_link(); }}); }
};

// Dedicated Client class wrapping SmppSession
class SmppClient {
    boost::asio::io_context io_context_;
    std::shared_ptr<SmppSession> session_;
    std::thread thread_;
public:
    // Callbacks
    std::function<void(std::shared_ptr<SmppPdu>)> on_request;
    std::function<void(std::shared_ptr<SmppPdu>)> on_response;
    std::function<void(uint32_t)> on_timeout;
    std::function<void()> on_close;

    SmppClient() : session_(std::make_shared<SmppSession>(io_context_)) {
        // bind session callbacks
        session_->on_request = [&](auto p){ if(on_request) on_request(p); };
        session_->on_response = [&](auto p){ if(on_response) on_response(p); };
        session_->on_timeout = [&](auto s){ if(on_timeout) on_timeout(s); };
        session_->on_close = [&](){ if(on_close) on_close(); };
    }

    // Connect and run in background
    void connect(const std::string& host, unsigned short port) {
        session_->connect(host, port);
        thread_ = std::thread([&]{ io_context_.run(); });
    }

    // Clean up
    void close() {
        session_->close();
        io_context_.stop();
        if(thread_.joinable()) thread_.join();
    }

    // Send arbitrary PDU
    void send_pdu(std::shared_ptr<SmppPdu> pdu, std::chrono::seconds timeout = std::chrono::seconds(30)) {
        session_->send_pdu(pdu, timeout);
    }
};

// Server: accepts multiple sessions
class SmppServer {
    boost::asio::io_context io_context_;
    tcp::acceptor acceptor_;
    std::vector<std::thread> thread_pool_;
public:
    SmppServer(unsigned short port, std::size_t threads)
      : io_context_(threads), acceptor_(io_context_, tcp::endpoint(tcp::v4(), port)) {
        start_accept();
        for(size_t i=0;i<threads;++i) thread_pool_.emplace_back([this]{ io_context_.run(); });
    }
    ~SmppServer(){ io_context_.stop(); for(auto& t:thread_pool_) t.join(); }
private:
    void start_accept() {
        auto session = std::make_shared<SmppSession>(io_context_);
        acceptor_.async_accept(session->socket(), [this, session](auto ec){ if(!ec) session->start(); start_accept(); });
    }
};

// Usage example
int main() {
    // Run server
    std::thread([]{ SmppServer server(2775, std::thread::hardware_concurrency()); }).detach();

    // Create client
    SmppClient client;
    client.on_request = [](auto p){ /* handle unsolicited */ };
    client.on_response = [](auto p){ /* handle responses */ };
    client.on_timeout = [](auto seq){ std::cerr<<"Timeout "<<seq<<"\n"; };
    client.on_close = []{ std::cerr<<"Disconnected\n"; };
    client.connect("127.0.0.1", 2775);

    // ... build and send a bind_transceiver PDU ...
    auto bind = std::make_shared<SmppPdu>(); // fill fields
    client.send_pdu(bind);

    // Run until done
    std::this_thread::sleep_for(std::chrono::seconds(120));
    client.close();
    return 0;
}
