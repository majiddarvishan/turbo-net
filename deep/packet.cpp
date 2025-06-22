// In packet.cpp
std::vector<uint8_t> Packet::serialize() const {
    std::vector<uint8_t> buffer(10 + body.size());

    // Header: [length(4)][type(1)][status(1)][seq_num(4)]
    buffer[0] = (length >> 24) & 0xFF;
    buffer[1] = (length >> 16) & 0xFF;
    buffer[2] = (length >> 8) & 0xFF;
    buffer[3] = length & 0xFF;
    buffer[4] = type;
    buffer[5] = status;
    buffer[6] = (seq_num >> 24) & 0xFF;
    buffer[7] = (seq_num >> 16) & 0xFF;
    buffer[8] = (seq_num >> 8) & 0xFF;
    buffer[9] = seq_num & 0xFF;

    // Body
    std::copy(body.begin(), body.end(), buffer.begin() + 10);
    return buffer;
}