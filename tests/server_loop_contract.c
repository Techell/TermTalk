/** @file server_loop_contract.c
 * @brief Лимит реестра, ожидание, изоляция переполнения и очистка TCP-ресурсов.
 */
#include "server_loop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s WSA=%d\n", __LINE__, #c, WSAGetLastError()); exit(1); } } while (0)
static char full[NETWORK_TX_CAPACITY];

static int Handle_Data(ServerLoop *loop, Client *sender, const char *data, int length, void *context)
{
    int *overflow = context;
    if (*overflow == 1) {
        /* Создаём переполнение ровно у одного получателя до следующего flush.
           Остальные должны получить x и остаться подключёнными. */
        CHECK(Socket_Send(sender, full, sizeof(full)) == 0);
        CHECK(ServerLoop_Broadcast(loop, NULL, "x", 1) == SERVER_CLIENT_LIMIT - 1);
        *overflow = 0;
        return 0;
    }
    if (Socket_Send(sender, data, length) < 0) return -1;
    if (*overflow == 2) {
        *overflow = 0;
        return Client_BeginDrain(sender);
    }
    return 0;
}

int main(void)
{
    ServerLoop *loop;
    Client peers[SERVER_CLIENT_LIMIT], extra = CLIENT_INITIALIZER;
    unsigned i;
    int overflow = 0;
    char byte;
    DWORD began;
    u_short port;
    SOCKET reopened = INVALID_SOCKET;
    CHECK(Network_Initialize() == 0);
    loop = ServerLoop_Create(0);
    CHECK(loop != NULL);
    port = ServerLoop_Port(loop);
    began = GetTickCount();
    CHECK(ServerLoop_Poll(loop, 100, Handle_Data, &overflow) == 0);
    CHECK(GetTickCount() - began >= 70);
    CHECK(ServerLoop_GetStats(loop).waits == 1 && ServerLoop_GetStats(loop).active == 0);
    for (i = 0; i < SERVER_CLIENT_LIMIT; ++i) {
        Client initial = CLIENT_INITIALIZER;
        peers[i] = initial; /* Копируется только пустой объект без ресурсов. */
        CHECK(Client_Initialize(&peers[i], "127.0.0.1", port) == 0);
        CHECK(ServerLoop_Poll(loop, 100, Handle_Data, &overflow) == 0);
    }
    CHECK(ServerLoop_GetStats(loop).active == SERVER_CLIENT_LIMIT);
    CHECK(Client_Initialize(&extra, "127.0.0.1", port) == 0);
    CHECK(ServerLoop_Poll(loop, 100, Handle_Data, &overflow) == 0);
    CHECK(ServerLoop_GetStats(loop).rejected == 1);
    Client_Close(&extra);
    overflow = 1;
    CHECK(send(peers[0].socket, "overflow", 8, 0) == 8);
    CHECK(ServerLoop_Poll(loop, 100, Handle_Data, &overflow) == 0);
    CHECK(ServerLoop_GetStats(loop).slow == 1);
    CHECK(ServerLoop_GetStats(loop).active == SERVER_CLIENT_LIMIT - 1);
    CHECK(ServerLoop_Poll(loop, 100, Handle_Data, &overflow) == 0);
    for (i = 1; i < SERVER_CLIENT_LIMIT; ++i)
        CHECK(Socket_Recv(&peers[i], &byte, 1) == 1 && byte == 'x');
    Client_Close(&peers[0]);

    /* Приложение может завершить отправку после ответа без входящего EOF. */
    CHECK(Client_Initialize(&extra, "127.0.0.1", port) == 0);
    CHECK(ServerLoop_Poll(loop, 100, Handle_Data, &overflow) == 0);
    overflow = 2;
    CHECK(send(extra.socket, "g", 1, 0) == 1);
    for (i = 0; i < 5; ++i) CHECK(ServerLoop_Poll(loop, 50, Handle_Data, &overflow) == 0);
    CHECK(Socket_Recv(&extra, &byte, 1) == 1 && byte == 'g');
    CHECK(Socket_Recv(&extra, &byte, 1) == 0);
    Client_Close(&extra);

    CHECK(Client_Initialize(&extra, "127.0.0.1", port) == 0);
    CHECK(ServerLoop_Poll(loop, 100, Handle_Data, &overflow) == 0);
    CHECK(ServerLoop_GetStats(loop).active == SERVER_CLIENT_LIMIT);
    CHECK(send(extra.socket, "e", 1, 0) == 1);
    CHECK(shutdown(extra.socket, SD_SEND) == 0);
    for (i = 0; i < 5; ++i) CHECK(ServerLoop_Poll(loop, 50, Handle_Data, &overflow) == 0);
    CHECK(Socket_Recv(&extra, &byte, 1) == 1 && byte == 'e');
    CHECK(Socket_Recv(&extra, &byte, 1) == 0);
    CHECK(ServerLoop_GetStats(loop).active == SERVER_CLIENT_LIMIT - 1);
    Client_Close(&extra);
    ServerLoop_Destroy(loop);
    /* Destroy закрывает оставшихся клиентов и listener, даже если они активны. */
    for (i = 1; i < SERVER_CLIENT_LIMIT; ++i) {
        fd_set readable;
        struct timeval wait = {3, 0};
        FD_ZERO(&readable); FD_SET(peers[i].socket, &readable);
        CHECK(select(0, &readable, NULL, NULL, &wait) > 0);
        CHECK(Socket_Recv(&peers[i], &byte, 1) == 0);
        Client_Close(&peers[i]);
    }
    CHECK(Server_Listen(&reopened, port) == 0);
    closesocket(reopened);
    Network_Cleanup();
    puts("server loop contract passed");
    return 0;
}
