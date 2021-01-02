#include <czmq.h>
#include "common.h"
#include "relay.h"

typedef struct _client {
    uint16_t identity;
    client_state state;
    int64_t next_heartbeat_time;
    int64_t expire_time;
} client;

struct _relay_server {
    zlist_t* clients;
    zhashx_t* session_ids;
    zsock_t* router;
    uint16_t next_session_id;

    zsock_t* fp;
    zsock_t* bp;
};

static size_t s_bernstein_hash_uint32(const void* key)
{
    uint32_t k = (uint32_t)key;
    const char* pointer = (const char*)&k;
    size_t key_hash = 0;
    size_t i = 0;
    for (i = 0; i < sizeof(uint32_t); i++) {
        key_hash = 33 * key_hash ^ *pointer++;
    }
    return key_hash;
}

static void* s_hash_key_dup(const void* key)
{
    return (void*)key;
}

static int s_hash_key_cmp(const void* key1, const void* key2)
{
    return key1 == key2 ? 0 : 1;
}

static int64_t check_latest_client_time(relay_server* s)
{
    client* c = (client*)zlist_first(s->clients);
    int64_t latest_time = INT64_MAX;
    while (c) {
        if (c->state == STA_CONNECTED && c->next_heartbeat_time < latest_time) {
            latest_time = c->next_heartbeat_time;
        }
        if (c->expire_time < latest_time) {
            latest_time = c->expire_time;
        }
        c = (client*)zlist_next(s->clients);
    }
    if (latest_time == INT64_MAX) {
        return -1;
    } else {
        int64_t now = zclock_time();
        return latest_time > now ? latest_time - now : 0;
    }
}

static client* s_index_client(relay_server* s, uint16_t identity, bool create_if_not_exist)
{
    client* c = (client*)zlist_first(s->clients);
    while (c) {
        if (c->identity == identity) {
            break;
        }
        c = (client*)zlist_next(s->clients);
    }
    if (!c && create_if_not_exist) {
        LOG_SERVER("I: create a new client=%d", identity);
        c = (client*)malloc(sizeof(client));
        if (c == NULL) {
            return NULL;
        }
        c->state = STA_WAITING_REGISTER;
        c->identity = identity;
        c->next_heartbeat_time = INT64_MAX;
        c->expire_time = zclock_time() + HEARTBEAT_TIMEOUT;
        zlist_push(s->clients, c);
    }
    return c;
}

static zframe_t* s_create_identity_frame(uint16_t identity)
{
    size_t sz = 1;
    if (identity > 256) {
        sz++;
    }
    zframe_t* idf = zframe_new(NULL, sz);
    byte* data = zframe_data(idf);
    if (sz == 2) {
        *(uint16_t*)data = identity;
    } else {
        *data = (byte)identity;
    }
    return idf;
}

static void s_broadcast(relay_server* s, zframe_t* f)
{
    client* loop_c = zlist_first(s->clients);
    while (loop_c) {
        if (loop_c->state == STA_CONNECTED) {
            zframe_t* idf = s_create_identity_frame(loop_c->identity);

            bool ok = false;
            if (zframe_send(&idf, s->router, ZFRAME_DONTWAIT | ZFRAME_MORE) != 0) {
            } else if (zframe_send(&f, s->router, ZFRAME_DONTWAIT | ZFRAME_REUSE) != 0) {
            } else {
                ok = true;
            }
            if (!ok) {
                LOG_SERVER("E: broadcast to client(%d) error", loop_c->identity);
            }
        }
        loop_c = zlist_next(s->clients);
    }
}

static bool s_unicast(relay_server* s, client* target_c, zframe_t** fs, size_t nf)
{
    zframe_t* idf = s_create_identity_frame(target_c->identity);

    if (zframe_send(&idf, s->router, ZFRAME_DONTWAIT | ZFRAME_MORE) != 0) {
        return false;
    }

    size_t i;
    for (i = 0; i < nf; i++) {
        int flags = ZFRAME_DONTWAIT | ZFRAME_REUSE;
        if (i != nf - 1) {
            flags |= ZFRAME_MORE;
        }
        if (zframe_send(&fs[i], s->router, flags) != 0) {
            return false;
        }
    }
    return true;
}

static void s_close_client(relay_server* s, client** c)
{
    if (!c || !*c) {
        return;
    }
    (*c)->state = STA_DISCONNECTED;
    zlist_remove(s->clients, (*c));
    free(*c);
    *c = NULL;
}

zframe_t* s_encode_clients_identity(relay_server* s)
{
    zframe_t* f = zframe_new(NULL, sizeof(uint16_t) * (1 + zlist_size(s->clients)));
    byte* buf = zframe_data(f);
    buf += 2;
    uint16_t valid_clients = 0;
    client* loop_c = zlist_first(s->clients);
    while (loop_c) {
        if (loop_c->state == STA_CONNECTED) {
            encode_uint16(buf, loop_c->identity);
            buf += 2;
            valid_clients++;
        }
        loop_c = zlist_next(s->clients);
    }
    buf = zframe_data(f);
    encode_uint16(buf, valid_clients);
    zframe_t* f2 = zframe_new(buf, sizeof(uint16_t) * (1 + valid_clients));
    zframe_destroy(&f);
    return f2;
}

static bool s_process_msg(relay_server* s, client* c, zmsg_t* msg)
{
    zframe_t* header = zmsg_pop(msg);
    if (header == NULL || zframe_size(header) != MSG_HEADER_SIZE) {
        LOG_SERVER("E: received an invalid size msg");
        return false;
    }

    msg_type mt;
    uint16_t target_identity;
    uint16_t session_id;
    decode_msg_header(header, &mt, &target_identity, &session_id);
    zframe_destroy(&header);

    c->expire_time = zclock_time() + HEARTBEAT_TIMEOUT;
    if (mt == MT_HEARTBEAT) {
        return true;
    }

    switch (c->state) {
    case STA_WAITING_REGISTER: {
        if (mt != MT_REGISTER) {
            LOG_SERVER("E: expect to receive an register msg");
            return false;
        }
        if (zmsg_size(msg) != 0) {
            LOG_SERVER("E: received an invalid size msg from client(%d)", c->identity);
            return false;
        }

        s->next_session_id++;

        // Response register
        zframe_t* fs[2];
        header = zframe_new(NULL, MSG_HEADER_SIZE);
        encode_msg_header(header, MT_REGISTERRSP, c->identity, s->next_session_id);
        fs[0] = header;
        fs[1] = s_encode_clients_identity(s);
        bool ok = s_unicast(s, c, fs, 2);
        zframe_destroy(&fs[0]);
        zframe_destroy(&fs[1]);
        if (!ok) {
            LOG_SERVER("E: sending the register rsp msg failed");
            return false;
        }
        c->state = STA_CONNECTED;
        c->next_heartbeat_time = HEARTBEAT_INTERVAL;

        // Update session id
        client* loop_c = (client*)zlist_first(s->clients);
        while (loop_c) {
            if (loop_c != c) {
                uint32_t sk = encode_session_key(loop_c->identity, c->identity);
                if (zhashx_lookup(s->session_ids, (void*)sk) != NULL) {
                    zhashx_update(s->session_ids, (void*)sk, (void*)s->next_session_id);
                } else {
                    zhashx_insert(s->session_ids, (void*)sk, (void*)s->next_session_id);
                    LOG_SERVER("DEBUGING: session_id(%d) id(%d)-id(%d) sk(%d) ", s->next_session_id, loop_c->identity, c->identity, sk);
                }
            }
            loop_c = (client*)zlist_next(s->clients);
        }

        // Broadcast the onregister msg
        loop_c = (client*)zlist_first(s->clients);
        client* next_c = NULL;
        header = zframe_new(NULL, MSG_HEADER_SIZE);
        encode_msg_header(header, MT_ONREGISTER, c->identity, s->next_session_id);
        while (loop_c) {
            next_c = (client*)zlist_next(s->clients);

            if (loop_c != c && loop_c->state == STA_CONNECTED) {
                if (!s_unicast(s, loop_c, &header, 1)) {
                    LOG_SERVER("E: sending the onregister msg to client(%d) faild, closed it", loop_c->identity);
                    s_close_client(s, &loop_c);
                }
            }
            loop_c = next_c;
        }
        zframe_destroy(&header);

        break;
    }
    case STA_CONNECTED: {
        if (mt != MT_NORMAL) {
            LOG_SERVER("E: received an unexpected from client(%d), msg type=%d", c->identity, mt);
            return false;
        }
        if (zmsg_size(msg) != 1) {
            LOG_SERVER("E: received an invalid size msg from client(%d)", c->identity);
            return false;
        }

        client* target_c = s_index_client(s, target_identity, false);
        if (target_c == NULL || target_c->state != STA_CONNECTED) {
            LOG_SERVER("E: sending the normal msg to target client(%d) failed which is disconnected", target_identity);
            return true;
        }

        uint32_t sk = encode_session_key(c->identity, target_c->identity);
        uint16_t cur_sid = (uint16_t)zhashx_lookup(s->session_ids, (void*)sk);
        if (cur_sid != session_id) { // 过期的会话消息
            LOG_SERVER("E: received an expired normal message(session=%d) from (%d) to (%d), current session=%d sk=%d", session_id, c->identity, target_identity, cur_sid, sk);
            return true;
        }

        // Forward
        zframe_t* fs[2];
        header = zframe_new(NULL, MSG_HEADER_SIZE);
        encode_msg_header(header, MT_NORMAL, c->identity, session_id);
        fs[0] = header;
        fs[1] = zmsg_pop(msg);
        if (!s_unicast(s, target_c, fs, 2)) {
            LOG_SERVER("E: sending the normal msg to client(%d) faild, closed it", target_c->identity);
            s_close_client(s, &target_c);
        }
        zframe_destroy(&fs[0]);
        zframe_destroy(&fs[1]);
        break;
    }
    default: {
        LOG_SERVER("E: client(%d) is in an unexpected state(%d)", c->identity, c->state);
        return false;
    }
    }
    return true;
}

static void s_process_timeout(relay_server* s)
{
    int64_t now = zclock_time();
    client* loop_c = zlist_first(s->clients);
    client* next_c = NULL;
    zframe_t* hf = NULL;
    while (loop_c) {
        next_c = zlist_next(s->clients);

        if (loop_c->state == STA_CONNECTED && loop_c->next_heartbeat_time <= now) {
            if (hf == NULL) {
                hf = zframe_new(NULL, MSG_HEADER_SIZE);
                encode_msg_header(hf, MT_HEARTBEAT, 0, 0);
            }
            if (!s_unicast(s, loop_c, &hf, 1)) {
                LOG_SERVER("E: sending the heartbeat msg to client(%d) failed", loop_c->identity);
                s_close_client(s, &loop_c);
            } else {
                // LOG_SERVER("I: send the hearbeat to client(%d)", loop_c->identity);
                loop_c->next_heartbeat_time = now + HEARTBEAT_INTERVAL;
            }
        }

        if (loop_c->expire_time <= now) {
            LOG_SERVER("I: client(%d) is closed since expired", loop_c->identity);

            zframe_t* cf = zframe_new(NULL, MSG_HEADER_SIZE);
            encode_msg_header(cf, MT_DISCONNECT, loop_c->identity, 0);
            s_broadcast(s, cf);
            zframe_destroy(&cf);
            s_close_client(s, &loop_c);
        }

        loop_c = next_c;
    }

    if (hf != NULL) {
        zframe_destroy(&hf);
    }
}

void server_loop(relay_server* s)
{
    zpoller_t* poller = zpoller_new(s->router, s->bp, NULL);
    assert(poller);

    while (true) {
        zsock_t* ready = (zsock_t*)zpoller_wait(poller, check_latest_client_time(s));
        if (ready == NULL) {
            if (zpoller_terminated(poller)) {
                LOG_SERVER("poller is terminated");
                break;
            }
            s_process_timeout(s);
            continue;
        }

        if (ready == s->bp) {
            if (zsock_wait(s->bp) != 0) {
                LOG_SERVER("zsock_wait failed");
            }
            break;
        }

        if (ready == s->router) {
            zmsg_t* msg = zmsg_recv(s->router);
            if (msg == NULL) {
                continue;
            }

            zframe_t* idf = zmsg_pop(msg);
            if (idf == NULL || zframe_size(idf) > sizeof(uint16_t)) {
                zmsg_destroy(&msg);
                zframe_destroy(&idf);
                LOG_SERVER("E: received an invalid msg");
                continue;
            }
            uint16_t identity;
            byte* data = zframe_data(idf);
            identity = *data;
            if (zframe_size(idf) > 1) {
                identity += (uint16_t)(*(data + 1)) << 8;
            }
            client* c = s_index_client(s, identity, true);
            zframe_destroy(&idf);

            if (!s_process_msg(s, c, msg)) {
                s_close_client(s, &c);
            }
            zmsg_destroy(&msg);
        } else {
            LOG_SERVER("E: unexpected");
        }
    }
    zlist_destroy(&s->clients);
    zhashx_destroy(&s->session_ids);
    zsock_destroy(&s->router);
    zsock_destroy(&s->bp);
    zsock_destroy(&s->fp);
    LOG_SERVER("server destroyed");
}

relay_server* create_server(const char* bind_address)
{
    relay_server* s = (relay_server*)malloc(sizeof(relay_server));
    if (s == NULL) {
        return s;
    }

    s->router = zsock_new_router(bind_address);
    if (!s->router) {
        LOG_SERVER("new socket failed, may the bind_address is invalid");
        return NULL;
    }
    s->clients = zlist_new();
    s->session_ids = zhashx_new();
    zhashx_set_key_hasher(s->session_ids, s_bernstein_hash_uint32);
    zhashx_set_key_destructor(s->session_ids, NULL);
    zhashx_set_key_duplicator(s->session_ids, s_hash_key_dup);
    zhashx_set_key_comparator(s->session_ids, s_hash_key_cmp);
    s->next_session_id = 0;
    assert(s->router);
    s->fp = zsys_create_pipe(&s->bp);
    assert(s->fp && s->bp);
    return s;
}

void destroy_server(relay_server** ps)
{
    if (!ps || !*ps) {
        return;
    }
    if (zsock_signal((*ps)->fp, 0) != 0) {
        LOG_SERVER("destroy server signal failed");
    }
    *ps = NULL;
}
