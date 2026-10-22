/** @file chat_server_contract.c
 * @brief Штатное уничтожение приложения с живыми сеансами и наблюдателями.
 */
#include "chat_server.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s WSA=%d\n", __LINE__, #c, WSAGetLastError()); exit(1); } } while (0)
typedef struct { int welcome, roster; } Results;

static int Observe(const ProtocolFrame *frame, void *context)
{
    Results *results = context;
    if (frame->type == PROTO_WELCOME) { ++results->welcome; CHECK(Protocol_Read32(frame->payload) != 0); }
    else if (frame->type == PROTO_LIST_RESPONSE) {
        ++results->roster; CHECK(Protocol_Read16(frame->payload) == 2);
    } else if (frame->type == PROTO_STATUS_RESPONSE) {
        CHECK(frame->request_id == 0 && Protocol_Read16(frame->payload + 2) == 10);
        CHECK(Protocol_Read64(frame->payload + 4) == PROTOCOL_FILE_DEFAULT);
    } else CHECK(0);
    return 0;
}

int main(void)
{
    ChatServer *server;
    Client peers[2] = {CLIENT_INITIALIZER, CLIENT_INITIALIZER};
    SOCKET reopened = INVALID_SOCKET;
    u_short port;
    unsigned i;
    CHECK(Network_Initialize() == 0);
    server = ChatServer_Create(0); CHECK(server != NULL);
    port = ChatServer_Port(server);
    for (i = 0; i < 2; ++i) {
        unsigned char frame[PROTOCOL_FRAME_MAX], hello[3] = {0,1,'A'};
        size_t length;
        CHECK(Client_Initialize(&peers[i], "127.0.0.1", port) == 0);
        hello[2] += (unsigned char)i;
        length = Protocol_Encode(frame, sizeof(frame), PROTO_HELLO, 1, hello, sizeof(hello));
        CHECK(length != 0 && Socket_Send(&peers[i], frame, (int)length) == 0);
        CHECK(Socket_Flush(&peers[i]) == 0);
    }
    CHECK(ChatServer_Poll(server, 10) == 0); /* Принимаем оба до чтения HELLO. */
    CHECK(ChatServer_Poll(server, 10) == 0);
    CHECK(ChatServer_Poll(server, 10) == 0);
    ChatServer_Destroy(server);
    for (i = 0; i < 2; ++i) {
        ProtocolDecoder decoder;
        Results results = {0};
        Protocol_Init(&decoder);
        for (;;) {
            fd_set readable;
            struct timeval wait = {3,0};
            char data[1024];
            int n;
            FD_ZERO(&readable); FD_SET(peers[i].socket, &readable);
            CHECK(select(0, &readable, NULL, NULL, &wait) > 0);
            n = Socket_Recv(&peers[i], data, sizeof(data)); CHECK(n >= 0);
            if (n == 0) break;
            CHECK(Protocol_Feed(&decoder, data, (size_t)n, Observe, &results) == 0);
        }
        CHECK(results.welcome == 1 && results.roster == 1 && Protocol_Finished(&decoder));
        Client_Close(&peers[i]);
    }
    CHECK(Server_Listen(&reopened, port) == 0); closesocket(reopened);
    Network_Cleanup();
    puts("chat server lifecycle passed");
    return 0;
}
