#pragma once
#include <vector>
#include <cstdint>

struct Packet {
    uint32_t length;        // Total packet length (header + body)
    uint8_t type;           // 0=Request, 1=Response
    uint8_t status;         // Additional status code
    uint32_t seq_num;       // Sequence number
    std::vector<uint8_t> body;

    // Serialize to byte buffer
    std::vector<uint8_t> serialize() const;

    // Deserialize from header + body
    static Packet deserialize(const uint8_t* header, const std::vector<uint8_t>& body);
};