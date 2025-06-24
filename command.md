Imagine you are senior software delevoper and want to develop a high performance tcp networking library in c++
It has below capabilities
Use boost asio
Has server and client
Client has reconnect mechanism
It consist of fixed size header and variable size body.
Header has 4 bytes of all packet length 1 byte packet type 1 byte status 4 bytes sequence number.
It has two types of packet Request and response that matches with sequence number.
When sends a request, wait for specific time in second to receive response, if it does not receive response call timeout function
Handle Fragmented TCP Packets
both client and server can send request and receive response
when client is run, it sends a packet that called bind-req, it has header and in the body sends id of client
server gives bind-req packet and generate bind-resp accoriding to it, it like bind-req structure
client send watchdog packet in specific period if it does not send any packet to server
packet type valid values are:
 0x01 : bind-req
 0x81 : bind-resp
 0x02 : stream-req
 0x82 : stream-resp
 0x03 : unbind-req
 0x83 : unbind-resp
 0x04 : watchdog-req
 0x84 : watchdog-resp