#include <stddef.h>
#include "relay.h"
#include <czmq.h>
#include "common.h"

#define CN 10
#define SA "tcp://127.0.0.1:12345"

typedef struct _client {
    uint16_t id;
    zactor_t* ca;
    uint16_t peers[CN];
    size_t pn;
} client;

typedef struct _server {
    relay_server* rs;
    zactor_t* sa;
} server;

client* AllClients[CN];

static void server_actor(zsock_t* pipe, void* args)
{
    zsock_signal(pipe, 0);
    relay_server* rs = (relay_server*)args;
    server_loop(rs);
}

static server setupServer(const char* bind_address)
{
    server s;
    s.rs = create_server(bind_address);
    assert(s.rs);
    s.sa = zactor_new(server_actor, s.rs);
    assert(s.sa);
    return s;
}

static void on_client_start(relay_client* rc)
{
    LOG_TEST("relay client(%d) start", client_identity(rc));
}

static void on_client_stop(relay_client* rc)
{
    LOG_TEST("relay client(%d) stop", client_identity(rc));
    client* c = AllClients[client_identity(rc) - 1];
    c->pn = 0;
}

static void on_peer_add(relay_client* rc, uint16_t peer)
{
    LOG_TEST("relay client(%d) add peer(%d)", client_identity(rc), peer);
    client* c = AllClients[client_identity(rc) - 1];
    int i;
    for (i = 0; i < c->pn; i++) {
        if (c->peers[i] == peer) {
            LOG_TEST("peer(%d) already added", peer);
            return;
        }
    }
    c->peers[c->pn] = peer;
    c->pn++;
}

static void on_peer_remove(relay_client* rc, uint16_t peer)
{
    LOG_TEST("relay client(%d) remove peer(%d)", client_identity(rc), peer);
    client* c = AllClients[client_identity(rc) - 1];
    int i;
    for (i = 0; i < c->pn; i++) {
        if (c->peers[i] == peer) {
            c->peers[i] = c->peers[c->pn - 1];
            c->pn--;
            return;
        }
    }
    LOG_TEST("peer(%d) removed failed: doesn't exist", peer);
}

static void on_msg_in(relay_client* rc, unsigned char* content, size_t sz, uint16_t identity)
{
    LOG_TEST("relay client(%d) received an msg(size=%d) from %d: %s", client_identity(rc), sz, identity, content);
}

static void client_actor(zsock_t* pipe, void* args)
{
    zsock_signal(pipe, 0);
    client* c = (client*)args;

    relay_callback cb;
    cb.on_start = on_client_start;
    cb.on_stop = on_client_stop;
    cb.on_peer_add = on_peer_add;
    cb.on_peer_remove = on_peer_remove;
    cb.on_msg_in = on_msg_in;
    relay_client* rc = create_client(c->id, SA, cb);
    assert(rc);

    zpoller_t* poller = zpoller_new(pipe, NULL);
    assert(poller);
    char content[1024];
    sprintf(content, "%d says hello world!", c->id);

    int i = 0;
    bool start = false;
    while (true) {
        zsock_t* ready = (zsock_t*)zpoller_wait(poller, 1000);
        if (ready == NULL) {
            if (zpoller_terminated(poller)) {
                break;
            }
            // timeout
            if (rc) {
                process_cmd(rc);
            }
            if (start && c->pn > 0 && i % 3 == 0) {
                size_t r = rand() % c->pn;
                send_msg(rc, c->peers[r], content, strlen(content) + 1);
            }
            i++;
            continue;
        }
        char* cstr = zstr_recv(ready);
        if (strcmp(cstr, "$TERM") == 0) {
            zstr_free(&cstr);
            break;
        } else if (strcmp(cstr, "START") == 0) {
            if (!rc) {
                rc = create_client(c->id, SA, cb);
            }
            start = true;
        } else if (strcmp(cstr, "STOP") == 0) {
            destroy_client(&rc);
            c->pn = 0;
            start = false;
        }
        zstr_free(&cstr);
    }
}

void TestRelay()
{
    LOG_TEST("test starting...");
    server s = setupServer(SA);

    int i;
    for (i = 0; i < CN; i++) {
        client* c = malloc(sizeof(client));
        c->id = i + 1;
        c->pn = 0;
        AllClients[i] = c;
        c->ca = zactor_new(client_actor, c);
    }

    for (i = 0; i < CN; i++) {
        zstr_send(AllClients[i]->ca, "START");
    }

    zpoller_t* poller = zpoller_new(NULL);

    client* sc = NULL;
    int64_t frame_count = 0;
    while (true) {
        zpoller_wait(poller, rand() % 20000 + 10000);
        if (zpoller_terminated(poller)) {
            break;
        }
        if (frame_count > 10 && s.rs && !sc) {
            LOG_TEST("the network has stabilized");
            continue;
        }
        if (frame_count % 3 == 0) {
            if (s.rs) {
                destroy_server(&s.rs);
                zactor_destroy(&s.sa);
                LOG_TEST("server destoyed");
            } else {
                s = setupServer(SA);
            }
        } else {
            if (!sc) {
                sc = AllClients[rand() % CN];
                zstr_send(sc->ca, "STOP");
            } else {
                zstr_send(sc->ca, "START");
                sc = NULL;
            }
        }
        frame_count++;
    }
    LOG_TEST("Test done...");
}

int main(int argc, char* argv[])
{
    TestRelay();
    return 0;
}
