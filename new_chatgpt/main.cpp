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

// Pre-allocate common buffer sizes
static constexpr size_t MAX_HEADER = 16;
static constexpr size_t MAX_PDU_SIZE = 1024 * 8; // adjust as needed

// SMPP PDU base
class SmppPdu {
public:
    uint32_t command_length;
    uint32_t command_id;
    uint32_t command_status;
    uint32_t sequence_number;
    std::vector<uint8_t> body;

    // Reserve capacity once
    SmppPdu() { body.reserve(256); }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf;
        buf.reserve(16 + body.size());
        // header
        uint32_t hdrs[4] = {
            boost::endian::native_to_big(static_cast<uint32_t>(16 + body.size())),
            boost::endian::native_to_big(command_id),
            boost::endian::native_to_big(command_status),
            boost::endian::native_to_big(sequence_number)
        };
        buf.insert(buf.end(), reinterpret_cast<uint8_t*>(hdrs), reinterpret_cast<uint8_t*>(hdrs+4));
        // body
        if (!body.empty()) buf.insert(buf.end(), body.begin(), body.end());
        return buf;
    }

    static std::shared_ptr<SmppPdu> deserialize(const uint8_t* data, size_t size) {
        if (size < MAX_HEADER) return nullptr;
        uint32_t cmd_len = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data));
        if (size < cmd_len || cmd_len > MAX_PDU_SIZE) return nullptr;
        auto pdu = std::make_shared<SmppPdu>();
        pdu->command_length = cmd_len;
        pdu->command_id = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data+4));
        pdu->command_status = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data+8));
        pdu->sequence_number = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data+12));
        pdu->body.assign(data+16, data+cmd_len);
        return pdu;
    }
};

class BindTransceiver : public SmppPdu {
public:
    BindTransceiver(uint32_t seq, const std::string& system_id, const std::string& password) {
        command_id = 0x00000009; // bind_transceiver
        command_status = 0;
        sequence_number = seq;
        // body: system_id\0 password\0 system_type\0 interface_version addr_ton addr_npi address_range\0
        body.insert(body.end(), system_id.begin(), system_id.end()); body.push_back(0);
        body.insert(body.end(), password.begin(), password.end()); body.push_back(0);
        body.push_back(0); // empty system_type
        body.push_back(0x34); // version 0x34
        body.push_back(0); // TON
        body.push_back(0); // NPI
        body.push_back(0); // address_range
    }
};

class BindTransceiverResp : public SmppPdu {
public:
    BindTransceiverResp(uint32_t seq, uint32_t status=0) {
        command_id = 0x80000009; // bind_transceiver_resp
        command_status = status;
        sequence_number = seq;
        std::string system_id = "SMPP-SERVER";
        body.insert(body.end(), system_id.begin(), system_id.end());
        body.push_back(0);
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
    boost::asio::io_context::strand strand_;
    boost::asio::streambuf streambuf_;
    asio_timer inactivity_timer_;
    boost::container::flat_map<uint32_t, std::unique_ptr<asio_timer>> pending_timers_;
    std::atomic<uint32_t> seq_{1};

public:
    // Callbacks
    std::function<void(std::shared_ptr<SmppPdu>)> on_request;
    std::function<void(std::shared_ptr<SmppPdu>)> on_response;
    std::function<void(uint32_t)> on_timeout;
    std::function<void()> on_close;

    SmppSession(boost::asio::io_context& ctx)
      : socket_(ctx)
      , strand_(ctx)
      , inactivity_timer_(ctx) {
        pending_timers_.reserve(128);
    }

    tcp::socket& socket() { return socket_; }

    void start() {
        reset_inactivity_timer();
        read_header();
    }

    void close() {
        boost::system::error_code ec;
        socket_.close(ec);
        if (on_close) on_close();
    }

    void send_pdu(const std::shared_ptr<SmppPdu>& pdu, std::chrono::seconds timeout = std::chrono::seconds(30)) {
        auto buf = pdu->serialize();
        auto self = shared_from_this();
        auto seq = pdu->sequence_number;

        // track timeout
        auto timer = std::make_unique<asio_timer>(socket_.get_executor().context(), timeout);
        timer->async_wait(boost::asio::bind_executor(strand_, [this, self, seq](auto ec){
            if (!ec) {
                pending_timers_.erase(seq);
                if (on_timeout) on_timeout(seq);
            }
        }));
        pending_timers_.emplace(seq, std::move(timer));

        // async write
        boost::asio::async_write(socket_, boost::asio::buffer(buf),
            boost::asio::bind_executor(strand_, [this, self](auto ec, auto){ if (ec) close(); }));
    }

private:
    void read_header() {
        auto self = shared_from_this();
        boost::asio::async_read(socket_, streambuf_, boost::asio::transfer_exactly(MAX_HEADER),
            boost::asio::bind_executor(strand_, [this, self](auto ec, auto){
                if (ec) { close(); return; }
                handle_header();
            }));
    }

    void handle_header() {
        auto data = boost::asio::buffer_cast<const uint8_t*>(streambuf_.data());
        uint32_t cmd_len = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(data));
        if (cmd_len < MAX_HEADER || cmd_len > MAX_PDU_SIZE) { close(); return; }
        read_body(cmd_len - MAX_HEADER);
    }

    void read_body(size_t size) {
        auto self = shared_from_this();
        boost::asio::async_read(socket_, streambuf_, boost::asio::transfer_exactly(size),
            boost::asio::bind_executor(strand_, [this, self, size](auto ec, auto){
                if (ec) { close(); return; }
                parse_pdu(size + MAX_HEADER);
                reset_inactivity_timer();
                read_header();
            }));
    }

    void parse_pdu(size_t total) {
        auto data = boost::asio::buffer_cast<const uint8_t*>(streambuf_.data());
        auto pdu = SmppPdu::deserialize(data, total);
        streambuf_.consume(total);
        if (!pdu) return;
        auto it = pending_timers_.find(pdu->sequence_number);
        if (it != pending_timers_.end()) {
            it->second->cancel();
            pending_timers_.erase(it);
            if (on_response) on_response(pdu);
        } else if (on_request) {
            on_request(pdu);
        }
    }

    void reset_inactivity_timer() {
        inactivity_timer_.expires_after(std::chrono::seconds(60));
        inactivity_timer_.async_wait(boost::asio::bind_executor(strand_, [this](auto ec){ if (!ec) close(); }));
    }
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

    void bind_transceiver(const std::string& sys_id,const std::string& pwd){ auto p=std::make_shared<BindTransceiver>(1,sys_id,pwd); send_pdu(p); }
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
     void start_accept()
     {
        auto sess=std::make_shared<SmppSession>(io_context_);
        sess->on_request=[sess](auto p){ if(p->command_id==0x00000009){ // bind_transceiver
                auto resp=std::make_shared<BindTransceiverResp>(p->sequence_number);
                sess->send_pdu(resp);
            } else {
                // other requests
            }
        };
        acceptor_.async_accept(sess->socket(),[this,sess](auto ec){ if(!ec) sess->start(); start_accept(); });
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
    client.bind_transceiver("sys","pwd");


    // ... build and send a bind_transceiver PDU ...
    auto bind = std::make_shared<SmppPdu>(); // fill fields
    client.send_pdu(bind);

    // Run until done
    std::this_thread::sleep_for(std::chrono::seconds(120));
    client.close();
    return 0;
}
