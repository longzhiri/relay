#ifndef RELAY_COMMON_H
#define RELAY_COMMON_h


#define HEARTBEAT_INTERVAL 5000
#define HEARTBEAT_TIMEOUT 10000

/********************************************************************************/
typedef enum _msg_type {
    MT_REGISTER,
    MT_REGISTERRSP,
    MT_ONREGISTER,
    MT_NORMAL,
    MT_HEARTBEAT,
    MT_DISCONNECT,
} msg_type;

typedef enum _client_state {
    STA_WAITING_REGISTER,
    STA_CONNECTED,
    STA_DISCONNECTED,
} client_state;

#define MSG_HEADER_SIZE 5 // msg type + identity + session id

void decode_msg_header(zframe_t* f, msg_type* mt, uint16_t* identity, uint16_t* session_id);
void encode_msg_header(zframe_t* f, msg_type mt, uint16_t identity, uint16_t session_id);

static uint16_t decode_uint16(byte* buf) {
    return (uint16_t)*buf << 8 | *(buf + 1);
}

static uint64_t decode_uint64(byte* buf) {
    return (uint64_t)*buf << 56 |
        ((uint64_t) * (buf + 1) << 48) |
        ((uint64_t) * (buf + 2) << 40) |
        ((uint64_t) * (buf + 3) << 32) |
        ((uint64_t) * (buf + 4) << 24) |
        ((uint64_t) * (buf + 5) << 16) |
        ((uint64_t) * (buf + 6) << 8) |
        ((uint64_t)*(buf+7));
}


static void encode_uint16(byte* buf, uint16_t val) {
    *buf = val >> 8;
    *(buf + 1) = val & 0xff;
}

static void encode_uint64(byte* buf, uint64_t val) {
    *buf++ = (byte)(val >> 56);
    *buf++ = (byte)(val >> 48);
    *buf++ = (byte)(val >> 40);
    *buf++ = (byte)(val >> 32);
    *buf++ = (byte)(val >> 24);
    *buf++ = (byte)(val >> 16);
    *buf++ = (byte)(val >> 8);
    *buf++ = (byte)val;
}

static uint64_t encode_session_key(uint16_t identity1, uint16_t identity2) {
    if (identity1 < identity2) {
        return (uint64_t)identity1 << 24 | (uint64_t)identity2 << 8;
    }
    else {
        return (uint64_t)identity2 << 24 | (uint64_t)identity1 << 8;
    }
}

#define LOG_SERVER(fmt, ...) zsys_error("[relay_server] %s:%d: "fmt, __FILE__, __LINE__ , ##__VA_ARGS__)
#define LOG_CLIENT(fmt, ...) zsys_error("[relay_client] %s:%d: "fmt, __FILE__, __LINE__ , ##__VA_ARGS__)
#define LOG_TEST(fmt, ...) zsys_error("[relay_client] %s:%d: "fmt, __FILE__, __LINE__ , ##__VA_ARGS__)
/********************************************************************************/


#endif