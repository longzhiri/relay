#ifndef _RELAY_h_
#define _RELAY_H_
#include <stdint.h>
typedef struct _relay_client relay_client;
typedef struct _relay_server relay_server;


typedef void (on_start_fn)(relay_client*);
typedef void (on_stop_fn)(relay_client*);
typedef void (on_peer_add_fn)(relay_client*, uint16_t);
typedef void (on_peer_remove_fn)(relay_client*, uint16_t);
typedef void (on_msg_in_fn)(relay_client* rc, unsigned char* content, size_t sz, uint16_t identity);

typedef struct _relay_callback {
    on_start_fn* on_start;
    on_stop_fn* on_stop;
    on_peer_add_fn* on_peer_add;
    on_peer_remove_fn* on_peer_remove;
    on_msg_in_fn* on_msg_in;
}relay_callback;


relay_client* create_client(uint16_t identity, const char* server_address, relay_callback cb);
void destroy_client(relay_client** prc);
uint16_t client_identity(relay_client* rc);
int send_msg(relay_client* rc, uint16_t t_identity, unsigned char* content, size_t sz);
void process_cmd(relay_client* rc);

relay_server* create_server(const char* bind_address);
void server_loop(relay_server* s);
void destroy_server(relay_server** ps);

#endif