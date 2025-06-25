#include "server.hpp"

#include <arpa/inet.h>
#include <map>

namespace hpnet {

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

} // namespace hpnet
