// SMPP High-Performance C++ Library with Fragmentation Handling and Callbacks
// Uses Boost.Asio for async networking and timers

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
        if (!body.empty()) {
            std::copy(body.begin(), body.end(), buf.begin() + 16);
        }
        return buf;
    }

    static std::shared_ptr<SmppPdu> deserialize(const std::vector<uint8_t>& buf) {
        if (buf.size() < 16) return nullptr;
        uint32_t cmd_len = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(buf.data()));
        if (buf.size() < cmd_len) return nullptr;
        auto pdu = std::make_shared<SmppPdu>();
        pdu->command_length = cmd_len;
        pdu->command_id = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(buf.data()+4));
        pdu->command_status = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(buf.data()+8));
        pdu->sequence_number = boost::endian::big_to_native(*reinterpret_cast<const uint32_t*>(buf.data()+12));
        pdu->body.assign(buf.begin()+16, buf.begin()+cmd_len);
        return pdu;
    }
};

// Enquire Link PDU
class EnquireLink : public SmppPdu {
public:
    EnquireLink(uint32_t seq) {
        command_id = 0x00000015; // enquire_link
        command_status = 0;
        sequence_number = seq;
    }
};

// Session: handles a single SMPP connection with fragmentation
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
      : socket_(ctx)
      , inactivity_timer_(ctx)
      , io_context_(ctx) {}

    tcp::socket& socket() { return socket_; }

    void start() {
        reset_inactivity_timer();
        do_read();
        schedule_enquire_link();
    }

    void close() {
        boost::system::error_code ec;
        socket_.close(ec);
        if (on_close) on_close();
    }

    void send_pdu(std::shared_ptr<SmppPdu> pdu, std::chrono::seconds timeout = std::chrono::seconds(30)) {
        uint32_t seq = pdu->sequence_number = seq_++;
        auto buf = pdu->serialize();
        auto self = shared_from_this();
        // start timeout timer
        asi o_timer;
        asio_timer timer(io_context_, timeout);
        pending_timers_.emplace(seq, std::move(timer));
        auto &t = pending_timers_[seq];
        t.async_wait([this, self, seq](const boost::system::error_code& ec) {
            if (!ec) {
                pending_timers_.erase(seq);
                if (on_timeout) on_timeout(seq);
            }
        });
        // write pdu
        boost::asio::async_write(socket_, boost::asio::buffer(buf),
            [this, self, seq](boost::system::error_code ec, std::size_t) {
                if (ec) close();
            });
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(
            std::back_inserter(recv_buffer_), 4096),
            [this, self](boost::system::error_code ec, std::size_t n) {
                if (ec) { close(); return; }
                reset_inactivity_timer();
                parse_buffer();
                do_read();
            });
    }

    void parse_buffer() {
        while (recv_buffer_.size() >= 16) {
            uint32_t cmd_len = boost::endian::big_to_native(
                *reinterpret_cast<const uint32_t*>(recv_buffer_.data()));
            if (recv_buffer_.size() < cmd_len) break;
            std::vector<uint8_t> pdu_buf(\cvbuffer_.begin(), recv_buffer_.begin()+cmd_len);
            recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin()+cmd_len);
            auto pdu = SmppPdu::deserialize(pdu_buf);
            if (!pdu) continue;
            auto it = pending_timers_.find(pdu->sequence_number);
            if (it != pending_timers_.end()) {
                it->second.cancel();
                pending_timers_.erase(it);
                if (on_response) on_response(pdu);
            } else if (on_request) {
                on_request(pdu);
            }
        }
    }

    void reset_inactivity_timer() {
        inactivity_timer_.expires_after(std::chrono::seconds(60));
        inactivity_timer_.async_wait([this](const boost::system::error_code& ec) {
            if (!ec) close();
        });
    }

    void schedule_enquire_link() {
        auto self = shared_from_this();
        auto timer = std::make_shared<asio_timer>(io_context_, std::chrono::seconds(30));
        timer->async_wait([this, self, timer](const boost::system::error_code& ec) {
            if (!ec) {
                auto el = std::make_shared<EnquireLink>(seq_++);
                send_pdu(el);
                schedule_enquire_link();
            }
        });
    }
};

// SMPP Client stub with fragmentation handling
class SmppClient : public std::enable_shared_from_this<SmppClient> {
    tcp::socket socket_;
    boost::asio::io_context& io_context_;
    std::vector<uint8_t> recv_buffer_;
    std::atomic<uint32_t> seq_{1};
    std::unordered_map<uint32_t, asio_timer> pending_timers_;

public:
    // Callbacks
    std::function<void(std::shared_ptr<SmppPdu>)> on_request;
    std::function<void(std::shared_ptr<SmppPdu>)> on_response;
    std::function<void(uint32_t)> on_timeout;
    std::function<void()> on_close;

    explicit SmppClient(boost::asio::io_context& ctx)
      : socket_(ctx)
      , io_context_(ctx) {}

    void connect(const std::string& host, unsigned short port) {
        socket_.connect({boost::asio::ip::address::from_string(host), port});
        do_read();
    }

    void send_request(std::shared_ptr<SmppPdu> pdu, std::chrono::seconds timeout = std::chrono::seconds(30)) {
        uint32_t seq = pdu->sequence_number = seq_++;
        auto buf = pdu->serialize();
        asio_timer timer(io_context_, timeout);
        pending_timers_.emplace(seq, std::move(timer));
        auto &t = pending_timers_[seq];
        t.async_wait([this, seq](const boost::system::error_code& ec) {
            if (!ec) {
                pending_timers_.erase(seq);
                if (on_timeout) on_timeout(seq);
            }
        });
        boost::asio::async_write(socket_, boost::asio::buffer(buf),
            [this](boost::system::error_code ec, std::size_t) {
                if (ec) close();
            });
    }

    void close() {
        boost::system::error_code ec;
        socket_.close(ec);
        if (on_close) on_close();
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(boost::asio::buffer(
            std::back_inserter(recv_buffer_), 4096),
            [this, self](boost::system::error_code ec, std::size_t n) {
                if (ec) { close(); return; }
                parse_buffer();
                do_read();
            });
    }

    void parse_buffer() {
        while (recv_buffer_.size() >= 16) {
            uint32_t cmd_len = boost::endian::big_to_native(
                *reinterpret_cast<const uint32_t*>(recv_buffer_.data()));
            if (recv_buffer_.size() < cmd_len) break;
            std::vector<uint8_t> pdu_buf(recv_buffer_.begin(), recv_buffer_.begin()+cmd_len);
            recv_buffer_.erase(recv_buffer_.begin(), recv_buffer_.begin()+cmd_len);
            auto pdu = SmppPdu::deserialize(pdu_buf);
            if (!pdu) continue;
            auto it = pending_timers_.find(pdu->sequence_number);
            if (it != pending_timers_.end()) {
                it->second.cancel();
                pending_timers_.erase(it);
                if (on_response) on_response(pdu);
            } else if (on_request) {
                on_request(pdu);
            }
        }
    }
};

// Example usage
int main() {
    SmppServer server(2775, std::thread::hardware_concurrency());
    return 0;
}
