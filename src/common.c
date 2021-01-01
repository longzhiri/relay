#include <czmq.h>
#include "common.h"

void decode_msg_header(zframe_t* f, msg_type* mt, uint16_t* identity, uint16_t* session_id) {
    assert(zframe_size(f) == MSG_HEADER_SIZE);
    byte* buf = zframe_data(f);
    *mt = *buf++;
    *identity = decode_uint16(buf);
    *session_id = decode_uint16(buf + 2);
}

void encode_msg_header(zframe_t* f, msg_type mt, uint16_t identity, uint16_t session_id) {
    assert(zframe_size(f) == MSG_HEADER_SIZE);
    byte* buf = zframe_data(f);
    *buf++ = mt;
    encode_uint16(buf, identity);
    buf += 2;
    encode_uint16(buf, session_id);
}
