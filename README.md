# relay

A message-oriented middleware decouples communication between nodes

All relay clients need to connect to the relay server instead of connecting to each other, the relay server router all messages between clients. The relay clients 
maintain a stateful connection session with each other, messages from an expired connection session will be discarded. Also the relay clients keepalive with each other and disconnect from one whose heartbeat has timed out.

## usage

```C
// Launch a relay server
relay_server* rs = create_server(bind_address);
sever_loop(rs);
destroy_server(rs);

// Launch a relay client

// The callbacks
void on_client_start(relay_client* rc); // A connection session start
void on_client_stop(relay_client* rc); // The connection session stop
void on_peer_add(relay_client* rc, uint16_t peer); // A peer client add
void on_peer_remove(relay_client* rc, uint16_t peer); // A peer client removed
void on_msg_in(relay_client* rc, unsigned char* content, size_t sz, uint16_t identity); // A msg in

relay_callback cb;
cb.on_start = on_client_start;
cb.on_stop = on_client_stop;
cb.on_peer_add = on_peer_add;
cb.on_peer_remove = on_peer_remove;
cb.on_msg_in = on_msg_in;
relay_client* rc = create_client(client_id,  server_address, cb); // Create an relay client
process_cmd(rc); // Non-blocking processing of messages once, may trigger a callback

send_message(rc, peer_id, "hello world", 12); // Send a message to the peer client

destroy_client(&rc); // Destroy the client

```