#include <czmq.h>
#include "common.h"
#include "relay.h"

#define REGISTER_TIMEOUT 5000

typedef struct _peer_client {
    uint16_t identity;
    client_state state;
    uint16_t session_id;
} peer_client;

struct _relay_client {
    uint16_t identity;
    client_state state;
    zlist_t* peers;

    zsock_t* conn;
    const char* server_address;

    int64_t next_heartbeat_time;
    int64_t expire_time;

    zactor_t* actor;
    zpoller_t* cmd_poller;

    relay_callback cb;

    bool destroyed;
};

typedef enum _cmd_type {
    CT_IN_DATA,
    CT_OUT_DATA,
    CT_PEER_ADD,
    CT_PEER_REMOVE,
    CT_CONNECTED,
    CT_DISCONNECTED,
} cmd_type;

#define CMD_SIZE 11

static void decode_cmd_header(zframe_t* f, cmd_type* ct, uint16_t* identity, zframe_t** pf)
{
    assert(zframe_size(f) == CMD_SIZE);
    byte* buf = zframe_data(f);
    *ct = *buf++;
    *identity = decode_uint16(buf);
    buf += 2;
    *pf = (zframe_t*)decode_uint64(buf);
}

static void encode_cmd_header(zframe_t* f, cmd_type ct, uint16_t identity, void* ptr)
{
    assert(zframe_size(f) == CMD_SIZE);
    byte* buf = zframe_data(f);
    *buf++ = ct;
    encode_uint16(buf, identity);
    buf += 2;
    encode_uint64(buf, (uint64_t)ptr);
}

static bool s_send_cmd(void* dest, cmd_type ct, uint16_t identity, zframe_t* data)
{
    zframe_t* f = zframe_new(NULL, CMD_SIZE);
    encode_cmd_header(f, ct, identity, data);
    int ret = zframe_send(&f, dest, ZFRAME_DONTWAIT);
    if (ret != 0) {
        zframe_destroy(&f);
    }
    return ret == 0;
}

static bool s_create_socket(relay_client* rc)
{
    assert(!rc->conn);
    rc->conn = zsock_new(ZMQ_DEALER);
    assert(rc->conn);

    int never_reconnect = -1;

    zmq_setsockopt(zsock_resolve(rc->conn), ZMQ_RECONNECT_IVL, &never_reconnect, sizeof(never_reconnect));

    size_t sz = 2;
    if (rc->identity < 256) {
        sz = 1;
    }
    zmq_setsockopt(zsock_resolve(rc->conn), ZMQ_IDENTITY, &rc->identity, sz);

    int ret = zsock_connect(rc->conn, rc->server_address);
    if (ret != 0) {
        LOG_CLIENT("E: Connect failed: invalid server address");
        return false;
    }
    return true;
}

static void s_destroy_socket(relay_client* rc)
{
    if (rc->conn) {
        zsock_destroy(&rc->conn);
    }
}

static bool s_send_frames(relay_client* rc, zframe_t** fs, size_t nf)
{
    size_t i;
    for (i = 0; i < nf; i++) {
        int flags = ZFRAME_DONTWAIT | ZFRAME_REUSE;
        if (i != nf - 1) {
            flags |= ZFRAME_MORE;
        }
        if (zframe_send(&fs[i], rc->conn, flags) != 0) {
            return false;
        }
    }
    return true;
}

static bool s_decode_peer_client_identities(relay_client* rc, zframe_t* f, uint16_t session_id)
{
    if (zframe_size(f) < sizeof(uint16_t)) {
        return false;
    }

    byte* buf = zframe_data(f);
    uint16_t n = decode_uint16(buf);
    if (zframe_size(f) != (n + 1) * sizeof(uint16_t)) {
        return false;
    }

    buf += 2;
    uint16_t i = 0;
    for (i = 0; i < n; i++) {
        peer_client* pc = (peer_client*)malloc(sizeof(peer_client));
        pc->identity = decode_uint16(buf);
        pc->state = STA_CONNECTED;
        pc->session_id = session_id;
        buf += 2;

        zlist_append(rc->peers, pc);
    }
    return true;
}

static int64_t s_check_latest_time(relay_client* rc)
{
    int64_t latest_time;
    if (rc->expire_time < rc->next_heartbeat_time) {
        latest_time = rc->expire_time;
    } else {
        latest_time = rc->next_heartbeat_time;
    }
    int64_t now = zclock_time();
    return latest_time > now ? latest_time - now : 0;
}

static bool s_start_client(relay_client* rc, zsock_t* pipe, bool* quit)
{
    if (!s_create_socket(rc)) {
        *quit = true;
        return false;
    }

    // Send register
    zframe_t* header = zframe_new(NULL, MSG_HEADER_SIZE);
    encode_msg_header(header, MT_REGISTER, rc->identity, 0);
    bool ok = s_send_frames(rc, &header, 1);
    zframe_destroy(&header);
    if (!ok) {
        LOG_CLIENT("E: sending register msg failed");
        return false;
    }
    rc->state = STA_WAITING_REGISTER;

    // Wait register rsp
    zpoller_t* poller = zpoller_new(rc->conn, NULL);
    assert(poller);
    zsock_t* ready = (zsock_t*)zpoller_wait(poller, REGISTER_TIMEOUT);
    if (ready == NULL) {
        LOG_CLIENT("E: wait to read the register rsp msg timeout");
        if (zsys_interrupted) {
            *quit = true;
        }
        return false;
    }
    zmsg_t* rsp_msg = NULL;
    rsp_msg = zmsg_recv(rc->conn);
    if (!rsp_msg) {
        LOG_CLIENT("E: received the register rsp msg failed");
        return false;
    }
    if (zmsg_size(rsp_msg) != 2) {
        LOG_CLIENT("E: recevived an invalid size register rsp msg");
        zmsg_destroy(&rsp_msg);
        return false;
    }

    zframe_t* hf = zmsg_pop(rsp_msg);
    zframe_t* cf = zmsg_pop(rsp_msg);
    zmsg_destroy(&rsp_msg);

    msg_type mt;
    uint16_t target_identity;
    uint16_t session_id;
    decode_msg_header(hf, &mt, &target_identity, &session_id);
    zframe_destroy(&hf);
    if (mt != MT_REGISTERRSP || target_identity != rc->identity) {
        LOG_CLIENT("E: recevied an unexpected msg");
        zframe_destroy(&cf);
        return false;
    }

    rc->peers = zlist_new();
    if (!s_decode_peer_client_identities(rc, cf, session_id)) {
        LOG_CLIENT("E: decode peer client identities failed");
        zlist_destroy(&(rc->peers));
        zframe_destroy(&cf);
        return false;
    }
    zframe_destroy(&cf);
    rc->state = STA_CONNECTED;
    rc->next_heartbeat_time = zclock_time() + HEARTBEAT_INTERVAL;
    rc->expire_time = zclock_time() + HEARTBEAT_TIMEOUT;

    if (!s_send_cmd(pipe, CT_CONNECTED, 0, NULL)) {
        LOG_CLIENT("E: sending the connected cmd failed");
    }

    peer_client* loop_pc = zlist_first(rc->peers);
    while (loop_pc) {
        s_send_cmd(pipe, CT_PEER_ADD, loop_pc->identity, NULL);
        loop_pc = zlist_next(rc->peers);
    }

    return true;
}

static void s_stop_client(relay_client* rc, zsock_t* pipe)
{
    s_destroy_socket(rc);

    if (rc->state == STA_CONNECTED) {
        rc->next_heartbeat_time = rc->expire_time = INT64_MAX;

        peer_client* loop_pc = zlist_first(rc->peers);
        while (loop_pc) {
            free(loop_pc);
            loop_pc = zlist_next(rc->peers);
        }
        zlist_destroy(&rc->peers);

        if (!s_send_cmd(pipe, MT_DISCONNECT, 0, NULL)) {
            LOG_CLIENT("E: sending the disconnected cmd failed");
        }
    }
    rc->state = STA_DISCONNECTED;
}

static peer_client* s_index_peer(relay_client* rc, uint16_t identity)
{
    peer_client* loop_p = zlist_first(rc->peers);
    while (loop_p) {
        if (loop_p->identity == identity) {
            return loop_p;
        }
        loop_p = zlist_next(rc->peers);
    }
    return NULL;
}

static bool s_msg_loop(relay_client* rc, zsock_t* pipe)
{
    zpoller_t* poller = zpoller_new(rc->conn, pipe, NULL);
    assert(poller);

    while (true) {
        zsock_t* ready = (zsock_t*)zpoller_wait(poller, s_check_latest_time(rc));
        if (ready == NULL) {
            if (zpoller_terminated(poller)) {
                return true;
            }
            if (rc->expire_time <= zclock_time()) {
                LOG_CLIENT("E: relay client=%d is expired", rc->identity);
                return false;
            }

            if (rc->next_heartbeat_time <= zclock_time()) {
                zframe_t* hf = zframe_new(NULL, MSG_HEADER_SIZE);
                encode_msg_header(hf, MT_HEARTBEAT, rc->identity, 0);
                if (!s_send_frames(rc, &hf, 1)) {
                    zframe_destroy(&hf);
                    return false;
                }
                zframe_destroy(&hf);
                rc->next_heartbeat_time += HEARTBEAT_INTERVAL;
            }
        } else if (ready == rc->conn) {
            zmsg_t* msg = zmsg_recv(rc->conn);
            if (msg == NULL) {
                continue;
            }
            if (zmsg_size(msg) < 1) {
                LOG_CLIENT("E: received an invalid size msg");
                return false;
            }

            rc->expire_time = zclock_time() + HEARTBEAT_TIMEOUT;

            zframe_t* hf = zmsg_pop(msg);
            msg_type mt;
            uint16_t src_identity;
            uint16_t session_id;
            decode_msg_header(hf, &mt, &src_identity, &session_id);
            zframe_destroy(&hf);

            switch (mt) {
            case MT_NORMAL: {
                if (zmsg_size(msg) != 1) {
                    zmsg_destroy(&msg);
                    LOG_CLIENT("E: received an invalid size msg");
                    return false;
                }

                peer_client* pc = s_index_peer(rc, src_identity);
                if (pc == NULL) {
                    LOG_CLIENT("E: recv an invalid session msg(session_id=%d) from the unknown peer(%d)", session_id, src_identity);
                    zmsg_destroy(&msg);
                    break;
                }
                if (pc->session_id != session_id) {
                    LOG_CLIENT("E: recv an invalid session msg(session_id=%d) from  the peer(%d), cur_session_id=%d", session_id, pc->identity, pc->session_id);
                    zmsg_destroy(&msg);
                    break;
                }
                zframe_t* cf = zmsg_pop(msg);
                zmsg_destroy(&msg);
                if (!s_send_cmd(pipe, CT_IN_DATA, src_identity, cf)) {
                    LOG_CLIENT("E: in msg was discarded since s_send_cmd failed");
                    zframe_destroy(&cf);
                }
                break;
            }
            case MT_DISCONNECT: {
                if (zmsg_size(msg) != 0) {
                    zmsg_destroy(&msg);
                    LOG_CLIENT("E: received an invalid size msg");
                    return false;
                }
                zmsg_destroy(&msg);

                if (src_identity == rc->identity) {
                    LOG_CLIENT("I: relay client is closed by remote relay server");
                    return false;
                }

                peer_client* loop_p = zlist_first(rc->peers);
                bool ok = false;
                while (loop_p) {
                    if (loop_p->identity == src_identity) {
                        zlist_remove(rc->peers, loop_p);
                        ok = true;
                        break;
                    }
                    loop_p = zlist_next(rc->peers);
                }
                if (!ok) {
                    LOG_CLIENT("E: no disconnected peer=%d  found", src_identity);
                } else {
                    s_send_cmd(pipe, CT_PEER_REMOVE, src_identity, NULL);
                }
                LOG_CLIENT("I: peer=%d is disconnected", src_identity);
                break;
            }
            case MT_HEARTBEAT: {
                if (zmsg_size(msg) != 0) {
                    zmsg_destroy(&msg);
                    LOG_CLIENT("E: received an invalid size msg");
                    return false;
                }
                zmsg_destroy(&msg);
                break;
            }
            case MT_ONREGISTER: {
                if (zmsg_size(msg) != 0) {
                    zmsg_destroy(&msg);
                    LOG_CLIENT("E: received an invalid size msg");
                    return false;
                }
                zmsg_destroy(&msg);

                if (src_identity == rc->identity) {
                    LOG_CLIENT("E: received an self-ONREGISTER msg");
                    break;
                }

                peer_client* pc = s_index_peer(rc, src_identity);
                if (pc == NULL) {
                    pc = (peer_client*)malloc(sizeof(peer_client));
                    pc->identity = src_identity;
                    pc->session_id = session_id;
                    pc->state = STA_CONNECTED;
                    zlist_append(rc->peers, pc);
                } else {
                    pc->session_id = session_id;
                    pc->state = STA_CONNECTED;
                }
                s_send_cmd(pipe, CT_PEER_ADD, src_identity, NULL);
                LOG_CLIENT("I: a new peer=%d add, session_id=%d", src_identity, session_id);
                break;
            }
            default: {
                LOG_CLIENT("E: received an unknown type msg");
                zmsg_destroy(&msg);
                return false;
            }
            }
        } else if (ready == pipe) {
            zmsg_t* msg = zmsg_recv(ready);
            if (!msg) {
                LOG_CLIENT("E: zmsg_recv failed");
                continue;
            }

            zframe_t* sf = zmsg_pop(msg);

            // Check quit
            size_t szTerm = strlen("$TERM");
            if (zframe_size(sf) == szTerm && memcmp(zframe_data(sf), "$TERM", szTerm) == 0) {
                zframe_destroy(&sf);
                zmsg_destroy(&msg);
                return true;
            }

            size_t szData = strlen("DATA");
            if (zframe_size(sf) != szData || memcmp(zframe_data(sf), "DATA", szData) != 0) {
                LOG_CLIENT("E: received an unknown type cmd");
                zframe_destroy(&sf);
                zmsg_destroy(&msg);
                continue;
            }

            zframe_destroy(&sf);

            if (zmsg_size(msg) != 1) {
                LOG_CLIENT("E: received an invalid size cmd");
                zmsg_destroy(&msg);
                continue;
            }
            zframe_t* f = zmsg_pop(msg);
            zmsg_destroy(&msg);

            cmd_type ct;
            uint16_t identity;
            zframe_t* df;
            decode_cmd_header(f, &ct, &identity, &df);
            zframe_destroy(&f);
            if (!df) {
                LOG_CLIENT("E: received an invalid data cmd, data frame is null");
                continue;
            }

            peer_client* pc = s_index_peer(rc, identity);
            if (!pc) {
                LOG_CLIENT("E: peer=%d doesn't exist", identity);
                zframe_destroy(&df);
                continue;
            }

            zframe_t* hf = zframe_new(NULL, MSG_HEADER_SIZE);
            encode_msg_header(hf, MT_NORMAL, identity, pc->session_id);
            zframe_t* fs[2];
            fs[0] = hf;
            fs[1] = df;
            bool ok = s_send_frames(rc, fs, 2);
            zframe_destroy(&hf);
            zframe_destroy(&df);
            if (!ok) {
                LOG_CLIENT("E: sending a normal msg failed");
                return false;
            }
        }
    }
}

static void s_client_actor(zsock_t* pipe, void* args)
{
    relay_client* rc = (relay_client*)args;
    zsock_signal(pipe, 0);

    bool quit = false;
    while (!quit) {
        switch (rc->state) {
        case STA_DISCONNECTED:
            if (!s_start_client(rc, pipe, &quit)) {
                s_stop_client(rc, pipe);
            }
            break;
        case STA_CONNECTED:
            quit = s_msg_loop(rc, pipe);
            s_stop_client(rc, pipe);
            break;
        default:
            quit = true;
            LOG_CLIENT("E: relay client is in unexpected state=%d", rc->state);
            break;
        }
    }
    LOG_CLIENT("I: relay client=%d actor quit...", rc->identity);
}

relay_client* create_client(uint16_t identity, const char* server_address, relay_callback cb)
{
    if (!cb.on_msg_in || !cb.on_peer_add || !cb.on_peer_remove
        || !cb.on_start || !cb.on_stop) {
        LOG_CLIENT("E: create client failed: the relay_callback is invalid");
        return NULL;
    }
    if (!server_address) {
        LOG_CLIENT("E: create client failed: the server_address is null");
        return NULL;
    }
    if (!identity) {
        LOG_CLIENT("E: create client failed: the identity can not be 0");
        return NULL;
    }
    relay_client* rc = (relay_client*)malloc(sizeof(relay_client));
    rc->identity = identity;
    rc->state = STA_DISCONNECTED;
    rc->peers = NULL;

    rc->server_address = server_address;
    rc->conn = NULL;
    rc->next_heartbeat_time = rc->expire_time = INT64_MAX;

    rc->actor = zactor_new(s_client_actor, rc);
    rc->cmd_poller = zpoller_new(rc->actor, NULL);

    rc->cb = cb;
    rc->destroyed = false;
    return rc;
}

void destroy_client(relay_client** prc)
{
    if (!prc || !*prc) {
        return;
    }
    if ((*prc)->destroyed) {
        return;
    }
    (*prc)->destroyed = true;
    zactor_destroy(&((*prc)->actor));
    zpoller_destroy(&((*prc)->cmd_poller));
    free(*prc);
    *prc = NULL;
}

uint16_t client_identity(relay_client* rc)
{
    assert(rc);
    if (!rc) {
        return 0;
    }
    return rc->identity;
}

int send_msg(relay_client* rc, uint16_t t_identity, unsigned char* content, size_t sz)
{
    if (rc->destroyed) {
        return -1;
    }
    if (zstr_sendm(rc->actor, "DATA") != 0) {
        LOG_CLIENT("E: send out msg failed: zstr_sendm failed");
        return -1;
    }
    zframe_t* df = zframe_new(content, sz);
    if (!s_send_cmd(rc->actor, CT_OUT_DATA, t_identity, df)) {
        zframe_destroy(&df);
        LOG_CLIENT("E: send out msg failed: s_send_cmd failed");
        return -1;
    }
    return 0;
}

void process_cmd(relay_client* rc)
{
    if (rc->destroyed) {
        return;
    }
    zactor_t* ready = (zactor_t*)zpoller_wait(rc->cmd_poller, 0);
    if (ready == NULL) {
        return;
    }
    if (ready != rc->actor) {
        LOG_CLIENT("E: an unexpected target actived");
        return;
    }
    zmsg_t* msg = zmsg_recv(ready);
    if (!msg) {
        LOG_CLIENT("E: zmsg_recv failed");
        return;
    }
    if (zmsg_size(msg) != 1) {
        LOG_CLIENT("E: received an invalid size cmd");
        zmsg_destroy(&msg);
        return;
    }
    zframe_t* f = zmsg_pop(msg);
    zmsg_destroy(&msg);

    cmd_type ct;
    uint16_t identity;
    zframe_t* df;
    decode_cmd_header(f, &ct, &identity, &df);

    switch (ct) {
    case CT_IN_DATA:
        if (!df) {
            LOG_CLIENT("E: null data frame found in data cmd");
            break;
        }
        rc->cb.on_msg_in(rc, (unsigned char*)zframe_data(df), zframe_size(df), identity);
        break;
    case CT_PEER_ADD:
        rc->cb.on_peer_add(rc, identity);
        break;
    case CT_PEER_REMOVE:
        rc->cb.on_peer_remove(rc, identity);
        break;
    case CT_CONNECTED:
        rc->cb.on_start(rc);
        break;
    case CT_DISCONNECTED:
        rc->cb.on_stop(rc);
        break;
    default:
        LOG_CLIENT("E: received an unknown type cmd=%d", ct);
        break;
    }
    zframe_destroy(&f);
    if (df) {
        zframe_destroy(&df);
    }
}
