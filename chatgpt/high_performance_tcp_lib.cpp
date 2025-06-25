// high_performance_tcp_lib.cpp
#include "high_performance_tcp_lib.hpp"
#include <arpa/inet.h>
#include <map>

namespace hpnet {

////////////////////////////////////////////////////////
// Connection Implementation
////////////////////////////////////////////////////////


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
    Header hdr = pkt.header;
    hdr.packet_length = htonl(hdr.packet_length);
    auto &bodyBuf = acquireBuffer(pkt.body.size());
    std::copy(pkt.body.begin(), pkt.body.end(), bodyBuf.begin());

    std::vector<boost::asio::const_buffer> bufs = {
        boost::asio::buffer(&hdr, sizeof(Header)),
        boost::asio::buffer(bodyBuf)
    };

    boost::asio::post(strand_, [this, self, bufs=std::move(bufs)]() mutable {
        bool writing = (count_ > 0);
        // flatten into one buffer for ring
        Buffer concat(sizeof(Header) + boost::asio::buffer_size(bufs[1]));
        std::memcpy(concat.data(), bufs[0].data(), sizeof(Header));
        std::memcpy(concat.data()+sizeof(Header), bufs[1].data(), boost::asio::buffer_size(bufs[1]));
        enqueueWrite(std::move(concat));
        if (!writing) doWrite();
    });
}

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

////////////////////////////////////////////////////////
// Server Implementation
////////////////////////////////////////////////////////

class Server::Impl {
public:
    Impl(boost::asio::io_context& ioc, const boost::asio::ip::tcp::endpoint& ep, Config cfg)
        : acceptor_(ioc, ep), config_(cfg), timeoutTimer_(ioc) {}

    void startAccept() {
        doAccept();
        scheduleTimeoutWheel();
    }

    void onBind(std::function<void(const std::string&)> cb) { bindHandler_ = std::move(cb); }

    void sendRequest(const std::string& clientId, const Packet& pkt,
                     std::function<void(const Packet&)> cb) {
        auto it = clients_.find(clientId); if (it==clients_.end()) return;
        uint32_t seq = nextSequence_++;
        Packet out = pkt; out.header.sequence = seq;
        out.header.packet_length = sizeof(Header)+out.body.size();
        pending_[seq] = cb;
        auto expireAt = std::chrono::steady_clock::now()+config_.request_timeout;
        timeoutWheel_[expireAt].push_back(seq);
        if (timeoutWheel_.begin()->first==expireAt) scheduleTimeoutWheel();
        it->second->sendPacket(out);
    }

private:
    using TimePoint = std::chrono::steady_clock::time_point;
    void doAccept() {
        acceptor_.async_accept([this](auto ec, auto sock){
            if(!ec) {
                auto conn = std::make_shared<Connection>(std::move(sock));
                conn->onRequest([this](auto&& p, auto c){ handleRequest(p,c); });
                conn->onResponse([this](auto&& r){ handleResponse(r); });
                conn->onError([](auto){});
                conn->start();
            }
            doAccept();
        });
    }
    void handleRequest(const Packet& req, ConnectionPtr conn) {
        if(req.header.type==PacketType::BindReq) {
            std::string id(req.body.begin(),req.body.end());
            clients_[id]=conn; if(bindHandler_) bindHandler_(id);
            Packet resp{req.header,req.body};
            resp.header.type=PacketType::BindResp; resp.header.status=Status::Ok;
            conn->sendPacket(resp);
        }
    }
    void handleResponse(const Packet& resp) {
        auto seq=resp.header.sequence;
        if(resp.header.type==PacketType::StreamResp||resp.header.type==PacketType::WatchdogResp) {
            if(auto it=pending_.find(seq);it!=pending_.end()){
                it->second(resp); pending_.erase(it);
            }
        }
    }
    void scheduleTimeoutWheel() {
        if(timeoutWheel_.empty()) return;
        timeoutTimer_.expires_at(timeoutWheel_.begin()->first);
        timeoutTimer_.async_wait([this](auto ec){
            if(ec) return;
            auto now=std::chrono::steady_clock::now();
            while(!timeoutWheel_.empty()&&timeoutWheel_.begin()->first<=now){
                for(auto seq:timeoutWheel_.begin()->second){
                    if(auto it=pending_.find(seq);it!=pending_.end()){it->second(Packet{}); pending_.erase(it);} }
                timeoutWheel_.erase(timeoutWheel_.begin());
            }
            if(!timeoutWheel_.empty()) scheduleTimeoutWheel();
        });
    }

    boost::asio::ip::tcp::acceptor acceptor_;
    Config config_;
    std::unordered_map<std::string,ConnectionPtr> clients_;
    std::unordered_map<uint32_t,std::function<void(const Packet&)>> pending_;
    std::map<TimePoint,std::vector<uint32_t>> timeoutWheel_;
    boost::asio::steady_timer timeoutTimer_;
    uint32_t nextSequence_{1};
    std::function<void(const std::string&)> bindHandler_;
};

Server::Server(boost::asio::io_context& ioc, const boost::asio::ip::tcp::endpoint& ep, Config cfg)
    : impl_(std::make_unique<Impl>(ioc,ep,cfg)) {}
Server::~Server() = default;

void Server::startAccept(){ impl_->startAccept(); }
void Server::onBind(std::function<void(const std::string&)> cb){ impl_->onBind(std::move(cb)); }

void Server::sendRequest(const std::string& clientId,const Packet& pkt,
                          std::function<void(const Packet&)>cb){
    impl_->sendRequest(clientId,pkt,cb);
}

////////////////////////////////////////////////////////
// Client Implementation with Timeout Wheel
////////////////////////////////////////////////////////

using TimePoint = std::chrono::steady_clock::time_point;

Client::Client(boost::asio::io_context& ioc,const boost::asio::ip::tcp::endpoint& ep,Config cfg)
    : socket_(ioc),io_context_(ioc),endpoint_(ep),
      reconnectTimer_(ioc),watchdogTimer_(ioc),timeoutTimer_(ioc),
      config_(cfg),nextSequence_(1){}
Client::~Client(){ stop(); }

void Client::start(){ scheduleTimeoutWheel(); doConnect(); }
void Client::stop(){ reconnectTimer_.cancel();watchdogTimer_.cancel();timeoutTimer_.cancel();boost::system::error_code ec;socket_.close(ec); }

void Client::onBindResp(std::function<void(const Packet&)> cb){ bindHandler_=std::move(cb);}
void Client::onError(ErrorHandler cb){ errorHandler_=std::move(cb);}

void Client::doConnect(){ auto self=shared_from_this(); socket_.async_connect(endpoint_,[this,self](auto ec){
    if(ec){ if(errorHandler_) errorHandler_(ec); scheduleReconnect();return;} sendBind(); auto conn=std::make_shared<Connection>(std::move(socket_)); conn->onResponse([this](auto&&p){handlePacket(p);}); conn->onError(errorHandler_); conn->start(); scheduleWatchdog();}); }

void Client::scheduleReconnect(){ reconnectTimer_.expires_after(config_.reconnect_interval); reconnectTimer_.async_wait([this](auto ec){ if(!ec) doConnect(); }); }

void Client::sendRequest(const Packet& pkt,std::function<void(const Packet&)>cb){ uint32_t seq=nextSequence_++; Packet out=pkt; out.header.sequence=seq; out.header.packet_length=sizeof(Header)+out.body.size(); pendingRequests_[seq]=cb; auto expireAt=std::chrono::steady_clock::now()+config_.request_timeout; timeoutWheel_[expireAt].push_back(seq); if(timeoutWheel_.begin()->first==expireAt) scheduleTimeoutWheel(); std::vector<uint8_t>buf(sizeof(Header)+out.body.size()); Header h=out.header; h.packet_length=htonl(h.packet_length); std::memcpy(buf.data(),&h,sizeof(Header)); std::copy(out.body.begin(),out.body.end(),buf.begin()+sizeof(Header)); boost::asio::async_write(socket_,boost::asio::buffer(buf),[this](auto ec,auto){ if(ec&&errorHandler_) errorHandler_(ec); }); }

void Client::scheduleTimeoutWheel(){ if(timeoutWheel_.empty()) return; timeoutTimer_.expires_at(timeoutWheel_.begin()->first); timeoutTimer_.async_wait([this](auto ec){ if(ec) return; auto now=std::chrono::steady_clock::now(); while(!timeoutWheel_.empty()&&timeoutWheel_.begin()->first<=now){ for(auto seq:timeoutWheel_.begin()->second){ if(auto it=pendingRequests_.find(seq);it!=pendingRequests_.end()){ it->second(Packet{}); pendingRequests_.erase(it);} } timeoutWheel_.erase(timeoutWheel_.begin()); } if(!timeoutWheel_.empty()) scheduleTimeoutWheel(); }); }

void Client::sendBind(){ Packet pkt; pkt.header.type=PacketType::BindReq; pkt.header.status=Status::Ok; std::string id="client-id"; pkt.body.assign(id.begin(),id.end()); sendRequest(pkt,[this](const Packet&resp){ if(bindHandler_) bindHandler_(resp); }); }

void Client::scheduleWatchdog(){ watchdogTimer_.expires_after(config_.watchdog_interval); watchdogTimer_.async_wait([this](auto ec){ if(!ec){ Packet pkt; pkt.header.type=PacketType::WatchdogReq; pkt.header.status=Status::Ok; sendRequest(pkt,[](auto){}); scheduleWatchdog(); } }); }

void Client::handlePacket(const Packet&pkt){ if(pkt.header.type==PacketType::BindResp){ if(bindHandler_) bindHandler_(pkt); } else if(auto it=pendingRequests_.find(pkt.header.sequence);it!=pendingRequests_.end()){ it->second(pkt); pendingRequests_.erase(it); } }

} // namespace hpnet
