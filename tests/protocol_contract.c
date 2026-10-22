/** @file protocol_contract.c
 * @brief Эталонные байты, все границы разреза, склейка и недопустимые кадры.
 */
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct { unsigned count; uint8_t type; uint32_t request; size_t length; unsigned char payload[PROTOCOL_PAYLOAD_MAX]; } Seen;

static int Observe(const ProtocolFrame *frame, void *context)
{
    Seen *seen = context;
    ++seen->count; seen->type = frame->type; seen->request = frame->request_id; seen->length = frame->length;
    memcpy(seen->payload, frame->payload, frame->length);
    return 0;
}

static int Reject(const ProtocolFrame *frame, void *context)
{
    (void)frame; (void)context; return -1;
}

static void Every_Split(const unsigned char *bytes, size_t n, uint8_t type, uint32_t request,
                        const unsigned char *payload, size_t length)
{
    size_t split;
    for (split = 0; split <= n; ++split) {
        ProtocolDecoder decoder;
        Seen seen = {0};
        Protocol_Init(&decoder);
        CHECK(Protocol_Feed(&decoder, bytes, split, Observe, &seen) == 0);
        CHECK(seen.count == (split == n ? 1u : 0u));
        CHECK(Protocol_Feed(&decoder, bytes + split, n - split, Observe, &seen) == 0);
        CHECK(seen.count == 1 && seen.type == type && seen.request == request && seen.length == length);
        CHECK(memcmp(seen.payload, payload, length) == 0 && Protocol_Finished(&decoder));
    }
}

int main(void)
{
    unsigned char frame[PROTOCOL_FRAME_MAX], copy[PROTOCOL_FRAME_MAX], payload[PROTOCOL_PAYLOAD_MAX];
    const unsigned char hello[] = {0,5,'A','l','i','c','e'};
    const unsigned char golden[] = {'W','S','C','K',2,1,0,0,0,0,0,7,0,0,0,1,0,5,'A','l','i','c','e'};
    const unsigned char russian[] = {0xd0,0x9f,0xd1,0x80,0xd0,0xb8,0xd0,0xb2,0xd0,0xb5,0xd1,0x82};
    const unsigned char invalid[][4] = {{0xc0,0x80,0,0},{0xe0,0x80,0xaf,0},{0xed,0xa0,0x80,0},
        {0xf4,0x90,0x80,0x80},{0x80,0,0,0},{0xc2,0x9b,0,0}};
    const size_t invalid_sizes[] = {2,3,3,4,1,2};
    ProtocolDecoder decoder;
    Seen seen = {0};
    size_t n, i;
    CHECK(Protocol_ValidText(russian, sizeof(russian), 1));
    CHECK(Protocol_ValidText((const unsigned char *)"\xe2\x82\xac", 3, 1));
    CHECK(Protocol_ValidText((const unsigned char *)"\xf4\x8f\xbf\xbf", 4, 1));
    for (i = 0; i < sizeof(invalid_sizes)/sizeof(invalid_sizes[0]); ++i)
        CHECK(!Protocol_ValidText(invalid[i], invalid_sizes[i], 0));
    CHECK(!Protocol_ValidText((const unsigned char *)"\0", 1, 0));
    CHECK(!Protocol_ValidText((const unsigned char *)"\x1b[31m", 5, 0));
    CHECK(Protocol_ValidText((const unsigned char *)"a\tb\nc", 5, 0));
    CHECK(!Protocol_ValidText((const unsigned char *)"a\tb", 3, 1));
    CHECK(!Protocol_ValidText(russian, sizeof(russian) - 1, 1));

    n = Protocol_Encode(frame, sizeof(frame), PROTO_HELLO, 1, hello, sizeof(hello));
    CHECK(n == sizeof(golden) && memcmp(frame, golden, n) == 0);
    Every_Split(frame, n, PROTO_HELLO, 1, hello, sizeof(hello));
    Protocol_Init(&decoder);
    for (i = 0; i < n; ++i) CHECK(Protocol_Feed(&decoder, frame + i, 1, Observe, &seen) == 0);
    CHECK(seen.count == 1 && decoder.frames_started == 1);
    /* Два кадра склеены, следующий заголовок оставлен незавершённым. */
    Protocol_Init(&decoder); seen.count = 0;
    {
        unsigned char joined[2 * sizeof(golden) + 5];
        memcpy(joined, frame, n); memcpy(joined + n, frame, n); memcpy(joined + 2 * n, frame, 5);
        CHECK(Protocol_Feed(&decoder, joined, sizeof(joined), Observe, &seen) == 0);
    }
    CHECK(seen.count == 2 && !Protocol_Finished(&decoder));
    for (i = 1; i < n; ++i) {
        Protocol_Init(&decoder);
        CHECK(Protocol_Feed(&decoder, frame, i, Observe, &seen) == 0);
        CHECK(!Protocol_Finished(&decoder));
    }
    /* Ошибки заголовка отклоняются до копирования payload. */
    for (i = 0; i < 5; ++i) {
        memcpy(copy, frame, n);
        if (i == 0) copy[0] = 'X';
        if (i == 1) copy[4] = 1;
        if (i == 2) copy[5] = 0x99;
        if (i == 3) copy[7] = 1;
        if (i == 4) Protocol_Write32(copy + 8, PROTOCOL_PAYLOAD_MAX + 1);
        Protocol_Init(&decoder);
        CHECK(Protocol_Feed(&decoder, copy, 16, Observe, &seen) == -1);
        CHECK(!Protocol_Finished(&decoder));
        CHECK(Protocol_Feed(&decoder, frame, n, Observe, &seen) == -1);
    }
    memcpy(copy, frame, n); copy[n - 1] = 0;
    Protocol_Init(&decoder);
    CHECK(Protocol_Feed(&decoder, copy, n, Observe, &seen) == -1);
    memcpy(copy, frame, n); copy[17] = 4;
    Protocol_Init(&decoder);
    CHECK(Protocol_Feed(&decoder, copy, n, Observe, &seen) == -1);
    Protocol_Init(&decoder);
    CHECK(Protocol_Feed(&decoder, frame, n, Reject, NULL) == -1);
    memset(copy, 0xa5, sizeof(copy));
    CHECK(Protocol_Encode(copy, n - 1, PROTO_HELLO, 1, hello, sizeof(hello)) == 0);
    CHECK(copy[0] == 0xa5);

    Protocol_Write16(payload, PROTOCOL_NICK_MAX);
    memset(payload + 2, 'n', PROTOCOL_NICK_MAX);
    n = Protocol_Encode(frame, sizeof(frame), PROTO_HELLO, 1, payload, 2 + PROTOCOL_NICK_MAX);
    CHECK(n == 16 + 2 + PROTOCOL_NICK_MAX);
    Every_Split(frame, n, PROTO_HELLO, 1, payload, 2 + PROTOCOL_NICK_MAX);

    Protocol_Write32(payload, 17); memset(payload + 4, 'x', PROTOCOL_TEXT_MAX);
    n = Protocol_Encode(frame, sizeof(frame), PROTO_CHAT_SEND, 0xffffffffu, payload, 4 + PROTOCOL_TEXT_MAX);
    CHECK(n == 16 + 4 + PROTOCOL_TEXT_MAX);
    Every_Split(frame, n, PROTO_CHAT_SEND, 0xffffffffu, payload, 4 + PROTOCOL_TEXT_MAX);
    CHECK(Protocol_Encode(copy, sizeof(copy), PROTO_CHAT_SEND, 2, payload, 4) == 0);
    Protocol_Write32(payload, 17); Protocol_Write32(payload + 4, 2);
    memset(payload + 8, 'y', PROTOCOL_TEXT_MAX);
    n = Protocol_Encode(frame, sizeof(frame), PROTO_CHAT_EVENT, 0, payload, 8 + PROTOCOL_TEXT_MAX);
    CHECK(n == 16 + 8 + PROTOCOL_TEXT_MAX);
    Every_Split(frame, n, PROTO_CHAT_EVENT, 0, payload, 8 + PROTOCOL_TEXT_MAX);
    CHECK(Protocol_Encode(copy, sizeof(copy), PROTO_CHAT_EVENT, 1, payload, sizeof(payload)) == 0);
    CHECK(Protocol_Encode(copy, sizeof(copy), 0xfe, 1, NULL, 0) == 0);
    CHECK(Protocol_Encode(copy, sizeof(copy), PROTO_PING, 2, NULL, 0) == 16);
    CHECK(Protocol_Encode(copy, sizeof(copy), PROTO_PING, 2, payload, 1) == 0);

    /* Список: точное исчерпание payload, статус, дубли ID, длина никнейма. */
    Protocol_Write16(payload, 1); Protocol_Write32(payload + 2, 1); payload[6] = 1;
    Protocol_Write16(payload + 7, 5); memcpy(payload + 9, "Alice", 5);
    payload[14] = 127; payload[15] = 0; payload[16] = 0; payload[17] = 1;
    Protocol_Write16(payload + 18, 12345);
    n = Protocol_Encode(frame, sizeof(frame), PROTO_LIST_RESPONSE, 0, payload, 20);
    CHECK(n == 36);
    Every_Split(frame, n, PROTO_LIST_RESPONSE, 0, payload, 20);
    CHECK(Protocol_Encode(frame, sizeof(frame), PROTO_LIST_RESPONSE, 0, payload, 21) == 0);
    payload[6] = 2;
    CHECK(Protocol_Encode(frame, sizeof(frame), PROTO_LIST_RESPONSE, 0, payload, 20) == 0);
    payload[6] = 1; memcpy(payload + 20, payload + 2, 18); Protocol_Write16(payload, 2);
    CHECK(Protocol_Encode(frame, sizeof(frame), PROTO_LIST_RESPONSE, 0, payload, 38) == 0);
    /* Размер и offset сохраняют старшие 32 бита; блок остаётся ограниченным. */
    Protocol_Write32(payload, 2); Protocol_Write64(payload + 4, PROTOCOL_FILE_DEFAULT);
    CHECK(Protocol_Read64(payload + 4) == UINT64_C(34359738368));
    Protocol_Write16(payload + 12, 1); payload[14] = 'a';
    n = Protocol_Encode(frame, sizeof(frame), PROTO_FILE_OFFER, 9, payload, 15);
    CHECK(n == 31); Every_Split(frame, n, PROTO_FILE_OFFER, 9, payload, 15);
    Protocol_Write64(payload + 4, PROTOCOL_FILE_MAX + 1);
    CHECK(!Protocol_Encode(frame, sizeof(frame), PROTO_FILE_OFFER, 9, payload, 15));
    Protocol_Write64(payload + 4, UINT64_C(4294967297));
    memset(payload + 12, 0xff, PROTOCOL_FILE_BLOCK);
    n = Protocol_Encode(frame, sizeof(frame), PROTO_FILE_DATA, 10, payload, sizeof(payload));
    CHECK(n == PROTOCOL_FRAME_MAX);
    Every_Split(frame, n, PROTO_FILE_DATA, 10, payload, sizeof(payload));
    Protocol_Write16(payload, 11); Protocol_Write16(payload + 2, 12); Protocol_Write64(payload + 4, 0);
    CHECK(Protocol_Encode(frame, sizeof(frame), PROTO_STATUS_RESPONSE, 1, payload, 12));
    Protocol_Write16(payload, 13);
    CHECK(!Protocol_Encode(frame, sizeof(frame), PROTO_STATUS_RESPONSE, 1, payload, 12));
    puts("protocol codec contract passed");
    return 0;
}
