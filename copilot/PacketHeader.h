#ifndef PACKETHEADER_H
#define PACKETHEADER_H

#include <cstdint>
#include <cstring>
#include <arpa/inet.h> // For htonl/ntohl. On Windows, include <winsock2.h> as needed.

struct PacketHeader {
    uint32_t packet_length; // Total packet length (header + body)
    uint8_t packet_type;    // E.g., 0x01 for request, 0x02 for response
    uint8_t status;         // Status code (if any)
    uint32_t sequence;      // Unique sequence number

    enum { header_length = 10 };

    // Serialize header into the provided buffer (network byte order)
    void to_buffer(char* data) const {
        uint32_t net_length = htonl(packet_length);
        uint32_t net_sequence = htonl(sequence);
        std::memcpy(data, &net_length, 4);
        data[4] = packet_type;
        data[5] = status;
        std::memcpy(data + 6, &net_sequence, 4);
    }

    // Deserialize header from the provided buffer (converts from network byte order)
    void from_buffer(const char* data) {
        std::memcpy(&packet_length, data, 4);
        packet_length = ntohl(packet_length);
        packet_type = data[4];
        status = data[5];
        std::memcpy(&sequence, data + 6, 4);
        sequence = ntohl(sequence);
    }
};

#endif // PACKETHEADER_H
