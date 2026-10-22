/** @file network_pressure.c
 * @brief Реальное заполнение TCP-буферов с последующим побайтовым восстановлением потока.
 */
#include "network.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s WSA=%d\n", __LINE__, #c, WSAGetLastError()); exit(1); } } while (0)

int main(void)
{
    Client sender = CLIENT_INITIALIZER, receiver = CLIENT_INITIALIZER;
    SOCKET listener = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_size = sizeof(address), buffer_limit = 4096, blocked = 0, i;
    unsigned long queued = 0, received = 0;
    DWORD started;
    static char data[NETWORK_TX_CAPACITY];
    char buffer[8192];
    CHECK(Network_Initialize() == 0);
    CHECK(Server_Listen(&listener, 0) == 0);
    CHECK(Server_Accept(listener, &receiver) == 1);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    CHECK(Client_Initialize(&sender, "127.0.0.1", ntohs(address.sin_port)) == 0);
    CHECK(Server_Accept(listener, &receiver) == 0);
    CHECK(setsockopt(sender.socket, SOL_SOCKET, SO_SNDBUF, (char *)&buffer_limit, sizeof(buffer_limit)) == 0);
    CHECK(setsockopt(receiver.socket, SOL_SOCKET, SO_RCVBUF, (char *)&buffer_limit, sizeof(buffer_limit)) == 0);
    for (i = 0; i < NETWORK_TX_CAPACITY; ++i) data[i] = (char)(i % 251);
    started = GetTickCount();
    /* Получатель намеренно не читает. Ждём настоящего WSAEWOULDBLOCK,
       а не предполагаем, что маленькая отправка обязательно будет частичной. */
    while (GetTickCount() - started < 5000 && queued < 64UL * 1024 * 1024) {
        int previous;
        if (sender.tx_size == 0) {
            CHECK(Socket_Send(&sender, data, sizeof(data)) == 0);
            queued += sizeof(data);
        }
        previous = sender.tx_size;
        CHECK(Socket_Flush(&sender) >= 0);
        if (sender.tx_size == previous) { blocked = 1; break; }
    }
    CHECK(blocked && Client_GetState(&sender) == CLIENT_OPEN);
    CHECK(Client_BeginDrain(&sender) == 0);
    started = GetTickCount();
    for (;;) {
        fd_set readable, writable;
        struct timeval wait = {0, 100000};
        int count;
        CHECK(GetTickCount() - started < 15000);
        FD_ZERO(&readable); FD_ZERO(&writable);
        FD_SET(receiver.socket, &readable);
        if (sender.tx_size > 0) FD_SET(sender.socket, &writable);
        else if (Client_GetState(&sender) == CLIENT_DRAINING) CHECK(Client_ShutdownSend(&sender) == 0);
        CHECK(select(0, &readable, &writable, NULL, &wait) != SOCKET_ERROR);
        if (FD_ISSET(sender.socket, &writable)) CHECK(Socket_Flush(&sender) >= 0);
        if (!FD_ISSET(receiver.socket, &readable)) continue;
        count = Socket_Recv(&receiver, buffer, sizeof(buffer));
        CHECK(count >= 0);
        if (count == 0) break;
        for (i = 0; i < count; ++i)
            CHECK(buffer[i] == data[(received + (unsigned)i) % NETWORK_TX_CAPACITY]);
        received += (unsigned)count;
    }
    CHECK(received == queued);
    Client_Close(&sender); Client_Close(&receiver); closesocket(listener);
    Network_Cleanup();
    printf("real backpressure passed: %lu bytes\n", received);
    return 0;
}
