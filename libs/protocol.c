/** @file protocol.c
 * @brief Побайтовый формат без отправки C-структур и без зависимости от выравнивания.
 */
#include "protocol.h"
#include <string.h>

uint16_t Protocol_Read16(const unsigned char *p) { return (uint16_t)((p[0] << 8) | p[1]); }
uint32_t Protocol_Read32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
void Protocol_Write16(unsigned char *p, uint16_t v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
void Protocol_Write32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

uint64_t Protocol_Read64(const unsigned char *p)
{ return ((uint64_t)Protocol_Read32(p) << 32) | Protocol_Read32(p + 4); }
void Protocol_Write64(unsigned char *p, uint64_t v)
{ Protocol_Write32(p, (uint32_t)(v >> 32)); Protocol_Write32(p + 4, (uint32_t)v); }

int Protocol_ValidText(const unsigned char *data, size_t length, int nickname)
{
    size_t i = 0;
    if (data == NULL && length != 0) return 0;
    while (i < length) {
        uint32_t cp, minimum;
        unsigned continuation, j;
        unsigned char first = data[i++];
        if (first < 0x80) {
            if (first == 0 || first == 0x7f || (first < 0x20
                && (nickname || (first != '\t' && first != '\n')))) return 0;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) { cp = first & 0x1f; continuation = 1; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { cp = first & 0x0f; continuation = 2; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { cp = first & 7; continuation = 3; minimum = 0x10000; }
        else return 0;
        if (length - i < continuation) return 0;
        for (j = 0; j < continuation; ++j) {
            unsigned char byte = data[i++];
            if ((byte & 0xc0) != 0x80) return 0;
            cp = (cp << 6) | (byte & 0x3f);
        }
        if (cp < minimum || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return 0;
        /* C1 также исключаем: в UTF-8 U+009B не должен стать управлением терминала. */
        if (cp >= 0x80 && cp <= 0x9f) return 0;
    }
    return 1;
}

int Protocol_ReadParticipant(const unsigned char *data, size_t length, size_t *offset,
                              ProtocolParticipant *participant)
{
    ProtocolParticipant item;
    size_t at, nick;
    if (data == NULL || offset == NULL || participant == NULL || *offset > length) return 0;
    at = *offset;
    if (length - at < 7) return 0;
    item.id = Protocol_Read32(data + at); item.status = data[at + 4];
    nick = Protocol_Read16(data + at + 5); at += 7;
    if (item.id == 0 || item.status != 1 || nick == 0 || nick > PROTOCOL_NICK_MAX
        || length - at < nick + 6 || !Protocol_ValidText(data + at, nick, 1)) return 0;
    memcpy(item.nickname, data + at, nick); item.nickname[nick] = 0; at += nick;
    memcpy(item.ipv4, data + at, 4); item.port = Protocol_Read16(data + at + 4);
    if (item.port == 0) return 0;
    *participant = item; *offset = at + 6;
    return 1;
}

static int Size_Allowed(uint8_t type, size_t n)
{
    switch (type) {
    case PROTO_HELLO: return n >= 3 && n <= 2 + PROTOCOL_NICK_MAX;
    case PROTO_WELCOME: case PROTO_ACCEPTED: return n == 4;
    case PROTO_LIST_REQUEST: case PROTO_PING: case PROTO_PONG: case PROTO_GOODBYE:
    case PROTO_STATUS_REQUEST: return n == 0;
    case PROTO_STATUS_RESPONSE: return n == 12;
    case PROTO_FILE_OFFER: return n >= 15 && n <= 14 + PROTOCOL_FILE_NAME_MAX;
    case PROTO_FILE_OFFER_EVENT: return n >= 19 && n <= 18 + PROTOCOL_FILE_NAME_MAX;
    case PROTO_FILE_CREATED: return n == 8;
    case PROTO_FILE_ACK: return n == 12;
    case PROTO_FILE_DECISION: case PROTO_FILE_DECISION_EVENT: case PROTO_FILE_CANCEL: return n == 5;
    case PROTO_FILE_DATA: return n >= 13 && n <= 12 + PROTOCOL_FILE_BLOCK;
    case PROTO_FILE_END: return n == 36;
    case PROTO_FILE_DONE: return n == 4;
    case PROTO_LIST_RESPONSE: return n >= 2 && n <= 2 + PROTOCOL_USERS_MAX * (13 + PROTOCOL_NICK_MAX);
    case PROTO_CHAT_SEND: return n >= 5 && n <= 4 + PROTOCOL_TEXT_MAX;
    case PROTO_CHAT_EVENT: return n >= 9 && n <= 8 + PROTOCOL_TEXT_MAX;
    case PROTO_ERROR: return n >= 4 && n <= 132;
    default: return 0;
    }
}

static int Frame_Valid(const ProtocolFrame *frame)
{
    const unsigned char *p = frame->payload;
    size_t n = frame->length;
    if (!Size_Allowed(frame->type, n) || (n != 0 && p == NULL)) return 0;
    switch (frame->type) {
    case PROTO_HELLO:
        return Protocol_Read16(p) == n - 2 && Protocol_ValidText(p + 2, n - 2, 1);
    case PROTO_WELCOME: case PROTO_ACCEPTED:
        return Protocol_Read32(p) != 0;
    case PROTO_CHAT_SEND:
        return Protocol_Read32(p) != 0 && Protocol_ValidText(p + 4, n - 4, 0);
    case PROTO_CHAT_EVENT:
        return frame->request_id == 0 && Protocol_Read32(p) != 0 && Protocol_Read32(p + 4) != 0
            && Protocol_ValidText(p + 8, n - 8, 0);
    case PROTO_ERROR:
        return Protocol_Read16(p) >= 1 && Protocol_Read16(p) <= 8
            && Protocol_Read16(p + 2) == n - 4 && Protocol_ValidText(p + 4, n - 4, 1);
    case PROTO_STATUS_RESPONSE:
        return Protocol_Read16(p + 2) >= 1 && Protocol_Read16(p + 2) <= PROTOCOL_USERS_MAX
            && Protocol_Read16(p) <= Protocol_Read16(p + 2) && Protocol_Read64(p + 4) <= PROTOCOL_FILE_MAX;
    case PROTO_FILE_OFFER: case PROTO_FILE_OFFER_EVENT: {
        size_t base = frame->type == PROTO_FILE_OFFER ? 0 : 4, i;
        const unsigned char *name = p + base + 14;
        size_t length = n - base - 14;
        if (!Protocol_Read32(p) || !Protocol_Read32(p + base) || Protocol_Read64(p + base + 4) > PROTOCOL_FILE_MAX
            || Protocol_Read16(p + base + 12) != length || !Protocol_ValidText(name, length, 1)
            || (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) return 0;
        for (i = 0; i < length; ++i) if (name[i] == '/' || name[i] == '\\' || name[i] == ':') return 0;
        return frame->type != PROTO_FILE_OFFER_EVENT || (Protocol_Read32(p + 4) && frame->request_id == 0);
    }
    case PROTO_FILE_CREATED: return Protocol_Read32(p) && Protocol_Read32(p + 4) && frame->request_id;
    case PROTO_FILE_DECISION: case PROTO_FILE_DECISION_EVENT:
        return Protocol_Read32(p) && p[4] <= 1;
    case PROTO_FILE_CANCEL: return Protocol_Read32(p) && p[4] >= 1 && p[4] <= 6;
    case PROTO_FILE_DATA: case PROTO_FILE_ACK: case PROTO_FILE_END: case PROTO_FILE_DONE:
        return Protocol_Read32(p) != 0;
    case PROTO_LIST_RESPONSE: {
        uint32_t ids[PROTOCOL_USERS_MAX];
        size_t at = 2, i, j, count = Protocol_Read16(p);
        ProtocolParticipant user;
        if (count > PROTOCOL_USERS_MAX) return 0;
        for (i = 0; i < count; ++i) {
            if (!Protocol_ReadParticipant(p, n, &at, &user)) return 0;
            for (j = 0; j < i; ++j) if (ids[j] == user.id) return 0;
            ids[i] = user.id;
        }
        return at == n;
    }
    default: return 1;
    }
}

size_t Protocol_Encode(void *output, size_t capacity, uint8_t type, uint32_t request_id,
                       const void *payload, size_t length)
{
    ProtocolFrame frame;
    unsigned char *p = output;
    frame.type = type; frame.request_id = request_id; frame.payload = payload; frame.length = length;
    if (output == NULL || length > PROTOCOL_PAYLOAD_MAX || capacity < PROTOCOL_HEADER_SIZE + length
        || !Frame_Valid(&frame)) return 0;
    memcpy(p, "WSCK", 4); p[4] = PROTOCOL_VERSION; p[5] = type; p[6] = p[7] = 0;
    Protocol_Write32(p + 8, (uint32_t)length); Protocol_Write32(p + 12, request_id);
    if (length != 0) memcpy(p + PROTOCOL_HEADER_SIZE, payload, length);
    return PROTOCOL_HEADER_SIZE + length;
}

void Protocol_Init(ProtocolDecoder *decoder)
{
    memset(decoder, 0, sizeof(*decoder));
    decoder->target = PROTOCOL_HEADER_SIZE;
}

int Protocol_Finished(const ProtocolDecoder *decoder)
{
    return decoder != NULL && !decoder->failed && decoder->used == 0;
}

int Protocol_Feed(ProtocolDecoder *decoder, const void *data, size_t length,
                  ProtocolHandler handler, void *context)
{
    const unsigned char *input = data;
    if (decoder == NULL || handler == NULL || (data == NULL && length != 0) || decoder->failed) return -1;
    while (length != 0) {
        size_t take = decoder->target - decoder->used;
        if (decoder->used == 0) ++decoder->frames_started;
        if (take > length) take = length;
        memcpy(decoder->bytes + decoder->used, input, take);
        decoder->used += take; input += take; length -= take;
        if (decoder->used == PROTOCOL_HEADER_SIZE && decoder->target == PROTOCOL_HEADER_SIZE) {
            size_t payload_size = Protocol_Read32(decoder->bytes + 8);
            if (memcmp(decoder->bytes, "WSCK", 4) != 0 || decoder->bytes[4] != PROTOCOL_VERSION
                || decoder->bytes[6] != 0 || decoder->bytes[7] != 0
                || payload_size > PROTOCOL_PAYLOAD_MAX || !Size_Allowed(decoder->bytes[5], payload_size)) {
                decoder->failed = 1; return -1;
            }
            decoder->target += payload_size;
        }
        if (decoder->used == decoder->target) {
            ProtocolFrame frame;
            frame.type = decoder->bytes[5]; frame.request_id = Protocol_Read32(decoder->bytes + 12);
            frame.payload = decoder->bytes + PROTOCOL_HEADER_SIZE;
            frame.length = decoder->target - PROTOCOL_HEADER_SIZE;
            if (!Frame_Valid(&frame) || handler(&frame, context) < 0) { decoder->failed = 1; return -1; }
            decoder->used = 0; decoder->target = PROTOCOL_HEADER_SIZE;
        }
    }
    return 0;
}
