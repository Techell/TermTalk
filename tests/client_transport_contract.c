/** @file client_transport_contract.c @brief Переполнение очереди событий завершает сеанс явно, сохраняя очередь. */
#include "client_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

int main(void)
{
    Client client = CLIENT_INITIALIZER;
    ClientTransport *transport;
    ClientEvent event;
    SOCKET listener, peer;
    struct sockaddr_in address = {0};
    int size = sizeof(address), error = 0, done = 0;
    unsigned char data[4096], payload[8];
    size_t at;
    unsigned i, count = 0;
    CHECK(Network_Initialize() == 0);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(listener != INVALID_SOCKET);
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &size) == 0 && listen(listener, 1) == 0);
    CHECK(Client_Initialize(&client, "127.0.0.1", ntohs(address.sin_port)) == 0);
    peer = accept(listener, NULL, NULL); CHECK(peer != INVALID_SOCKET);
    transport = Transport_Create(&client); CHECK(transport);
    Protocol_Write32(payload, 1); at = Protocol_Encode(data, sizeof(data), PROTO_WELCOME, 1, payload, 4);
    for (i = 0; i < 200; ++i) at += Protocol_Encode(data + at, sizeof(data) - at, PROTO_PONG, i + 2, NULL, 0);
    CHECK(send(peer, (const char *)data, (int)at, 0) == (int)at);
    for (i = 0; i < 400; ++i) { if ((done = Transport_Done(transport, &error))) break; Sleep(5); }
    CHECK(done && error == WSAENOBUFS);
    while (Transport_Pop(transport, &event)) {
        CHECK(event.type == (count ? PROTO_PONG : PROTO_WELCOME)); CHECK(event.request == count + 1); ++count;
    }
    CHECK(count == 64 && !Transport_Post(transport, PROTO_PING, 1, NULL, 0));
    Transport_Destroy(transport); CHECK(Client_GetState(&client) == CLIENT_OPEN);
    Client_Close(&client); closesocket(peer); closesocket(listener); Network_Cleanup();
    puts("PASS: bounded incoming event queue, explicit overflow, ordered copies and borrowed socket ownership"); return 0;
}
