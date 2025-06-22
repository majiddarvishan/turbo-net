Client client(io_context, "127.0.0.1", "12345");
client.connect();

Packet req;
req.type = 0; // Request
req.body = { 'D', 'A', 'T', 'A' };

client.async_request(
    std::move(req),
    [](Packet resp) { /* Handle response */ },
    []() { std::cerr << "Timeout!\n"; },
    5 // Timeout after 5 seconds
);