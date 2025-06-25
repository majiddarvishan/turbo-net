#include "client.hpp"

#include <arpa/inet.h>
#include <map>

namespace hpnet {

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
