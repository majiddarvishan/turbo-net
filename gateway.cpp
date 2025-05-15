#include "turbonet_server.h"
#include "turbonet_client.h"
#include <iostream>
#include <unordered_map>
#include <mutex>
#include <thread>

using namespace turbonet;

int main()
 {
    // 1. Incoming server: listens for external producers on port 8000
    TurboNetServer server(8000, /*maxConns=*/50);

    // 2. Outgoing client: connects to downstream processor on localhost:9000
    auto client = std::make_shared<TurboNetClient>(5000, 5000, 10000);

    // Map originalSeq -> gateway-generated seq for response correlation
    std::mutex mapMutex;
    std::unordered_map<uint32_t, uint32_t> seqMap;

    // 3. When client gets a response, forward it back to original producer
    client->setPacketHandler(
        [&](uint8_t packetId, uint8_t status, uint32_t cseq, const std::vector<uint8_t>& payload) {
            std::lock_guard<std::mutex> lk(mapMutex);
            // find original sequence
            auto it = std::find_if(seqMap.begin(), seqMap.end(),
                                   [&](auto &p){ return p.second == cseq; });
            if (it != seqMap.end()) {
                uint32_t origSeq = it->first;
                seqMap.erase(it);
                // send back to producer via server’s respond lambda
                // Note: we can store per-session respond lambdas in Session or pass through
                // For simplicity, we echo to all connected sessions:
                server.start([&](uint8_t, uint8_t, uint32_t, const std::vector<uint8_t>&, auto respond){
                    respond(packetId, status, origSeq, payload);
                });
            }
        });

    // Connect client immediately
    client->connect("127.0.0.1", 9000, 3000, [&](const asio::error_code& ec){
        if (ec) std::cerr << "Gateway→client connect failed: " << ec.message() << "\n";
        else    std::cout << "Gateway connected to downstream\n";
    });

    // 4. Server request handler: forward to client
    server.setAuthHandler([](const std::string&){ return true; }); // skip auth
    server.start(
        [&](uint8_t pid, uint8_t status, uint32_t seq,
            const std::vector<uint8_t>& payload,
            std::function<void(uint8_t,uint8_t,uint32_t,const std::vector<uint8_t>&)> respond)
    {
        // Only forward request packets (0x02)
        if (pid == 0x02) {
            // sendRequest returns new sequence
            uint32_t newSeq = client->sendRequest(payload.data(), payload.size());
            {
                std::lock_guard<std::mutex> lk(mapMutex);
                seqMap[seq] = newSeq;
            }
            // we will reply later via client callback
        }
        else {
            // ignore or handle other packet types directly
            respond(pid, status, seq, payload);
        }
    });

    std::cout << "Gateway running. Press Enter to quit.\n";
    std::cin.get();

    server.stop();
    client->close();
    return 0;
}
