Server server(io_context, 12345);
server.set_request_handler([](Packet req) -> Packet {
    Packet resp;
    resp.type = 1; // Response
    resp.seq_num = req.seq_num;
    resp.body = { 'O', 'K' }; // Echo body
    return resp;
});