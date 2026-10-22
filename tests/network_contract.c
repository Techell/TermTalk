/** @file network_contract.c
 * @brief Проверки состояний и владения на реальном TCP loopback.
 */
#include "network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "line %d: %s (WSA=%d)\n", __LINE__, #condition, WSAGetLastError()); \
    exit(1); } } while (0)

int main(void)
{
    Client client = CLIENT_INITIALIZER;
    SOCKET listener, peer, original;
    struct sockaddr_in address = {0};
    int address_size = sizeof(address), result, total = 0;
    char data[NETWORK_TX_CAPACITY], received[NETWORK_TX_CAPACITY];
    DWORD timeout = 3000;
    CHECK(Network_Initialize() == 0);
    CHECK(Client_GetState(&client) == CLIENT_CLOSED);
    CHECK(Socket_Send(&client, "x", 1) == -1);
    CHECK(Client_BeginDrain(&client) == -1);
    CHECK(Client_ShutdownSend(&client) == -1);
    CHECK(Network_ClassifyError(WSAEWOULDBLOCK) == NET_ERROR_WAIT);
    CHECK(Network_ClassifyError(WSAECONNRESET) == NET_ERROR_DISCONNECTED);
    CHECK(Network_ClassifyError(WSAECONNREFUSED) == NET_ERROR_REFUSED);
    CHECK(Network_ClassifyError(WSAETIMEDOUT) == NET_ERROR_TIMEOUT);
    CHECK(Network_ClassifyError(WSAEHOSTUNREACH) == NET_ERROR_UNREACHABLE);
    CHECK(Network_ClassifyError(123456) == NET_ERROR_SYSTEM);
    CHECK(Network_ClassifyLocalError(WSAEADDRINUSE) == NET_LOCAL_ADDRESS_IN_USE);
    CHECK(Network_ClassifyLocalError(WSAEACCES) == NET_LOCAL_ACCESS);
    CHECK(Network_ClassifyLocalError(WSAENOBUFS) == NET_LOCAL_RESOURCES);
    CHECK(Network_ClassifyLocalError(WSANOTINITIALISED) == NET_LOCAL_NOT_INITIALIZED);
    CHECK(Network_ClassifyLocalError(WSAEINVAL) == NET_LOCAL_ARGUMENT);
    CHECK(Network_ClassifyLocalError(WSAESHUTDOWN) == NET_LOCAL_STATE);

    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    CHECK(listener != INVALID_SOCKET);
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &address_size) == 0);
    CHECK(listen(listener, 1) == 0);
    CHECK(Client_Initialize(&client, "127.0.0.1", ntohs(address.sin_port)) == 0);
    peer = accept(listener, NULL, NULL);
    CHECK(peer != INVALID_SOCKET);
    CHECK(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout)) == 0);
    CHECK(Client_GetState(&client) == CLIENT_OPEN);
    CHECK(Socket_Recv(&client, received, sizeof(received)) == SOCKET_RECV_WOULD_BLOCK);
    CHECK(Client_GetState(&client) == CLIENT_OPEN);

    /* Повторная инициализация не должна терять прежний дескриптор. */
    original = client.socket;
    CHECK(Client_Initialize(&client, "127.0.0.1", ntohs(address.sin_port)) == -1);
    CHECK(WSAGetLastError() == WSAEISCONN && client.socket == original);
    CHECK(Server_Accept(listener, &client) == -1);
    CHECK(client.socket == original);

    memset(data, 'a', sizeof(data));
    CHECK(Socket_Send(&client, NULL, 0) == 0);
    CHECK(Socket_Send(&client, NULL, 1) == -1);
    CHECK(client.tx_size == 0);
    CHECK(Socket_Send(&client, data, sizeof(data)) == 0);
    CHECK(Socket_Send(&client, "b", 1) == SOCKET_QUEUE_FULL);
    CHECK(client.tx_size == sizeof(data) && memcmp(client.tx_buffer, data, sizeof(data)) == 0);
    CHECK(Client_GetState(&client) == CLIENT_OPEN);
    CHECK(Client_BeginDrain(&client) == 0);
    CHECK(Client_BeginDrain(&client) == 0);
    CHECK(Socket_Send(&client, "b", 1) == -1 && WSAGetLastError() == WSAENOTCONN);
    CHECK(Client_ShutdownSend(&client) == -1); /* Не терять очередь при завершении. */
    do {
        fd_set writable;
        struct timeval wait = {3, 0};
        FD_ZERO(&writable);
        FD_SET(client.socket, &writable);
        CHECK(select(0, NULL, &writable, NULL, &wait) > 0);
        result = Socket_Flush(&client);
        CHECK(result >= 0);
    } while (result != 0);
    CHECK(Client_ShutdownSend(&client) == 0);
    CHECK(Client_ShutdownSend(&client) == 0);
    CHECK(Client_GetState(&client) == CLIENT_SEND_CLOSED);
    while (total < NETWORK_TX_CAPACITY) {
        result = recv(peer, received + total, NETWORK_TX_CAPACITY - total, 0);
        CHECK(result > 0);
        total += result;
    }
    CHECK(memcmp(received, data, sizeof(data)) == 0);
    CHECK(recv(peer, received, sizeof(received), 0) == 0);
    CHECK(send(peer, "reply", 5, 0) == 5);
    CHECK(shutdown(peer, SD_SEND) == 0);
    total = 0;
    do {
        fd_set readable;
        struct timeval wait = {3, 0};
        FD_ZERO(&readable);
        FD_SET(client.socket, &readable);
        CHECK(select(0, &readable, NULL, NULL, &wait) > 0);
        result = Socket_Recv(&client, received + total, sizeof(received) - total);
        CHECK(result >= 0);
        total += result;
    } while (result != 0);
    CHECK(total == 5 && memcmp(received, "reply", 5) == 0);
    CHECK(Client_HasPeerEOF(&client));
    CHECK(Socket_Recv(&client, received, sizeof(received)) == 0);
    Client_Close(&client);
    Client_Close(&client);
    CHECK(Client_GetState(&client) == CLIENT_CLOSED && client.socket == INVALID_SOCKET);
    CHECK(!Client_HasPeerEOF(&client) && client.tx_size == 0);
    closesocket(peer);

    /* Повторное использование после Close допустимо. Аварийный RST не должен
       закрывать дескриптор из функции приёма: это обязанность владельца. */
    CHECK(Client_Initialize(&client, "127.0.0.1", ntohs(address.sin_port)) == 0);
    peer = accept(listener, NULL, NULL);
    CHECK(peer != INVALID_SOCKET);
    {
        struct linger reset = {1, 0};
        fd_set readable;
        struct timeval wait = {3, 0};
        CHECK(setsockopt(peer, SOL_SOCKET, SO_LINGER, (char *)&reset, sizeof(reset)) == 0);
        closesocket(peer);
        FD_ZERO(&readable);
        FD_SET(client.socket, &readable);
        CHECK(select(0, &readable, NULL, NULL, &wait) > 0);
        original = client.socket;
        CHECK(Socket_Recv(&client, received, sizeof(received)) == -1);
        CHECK(Network_ClassifyError(WSAGetLastError()) == NET_ERROR_DISCONNECTED);
        CHECK(Client_GetState(&client) == CLIENT_FAILED && client.socket == original);
        CHECK(Socket_Send(&client, "x", 1) == -1);
        Client_Close(&client);
    }
    closesocket(listener);
    Network_Cleanup();
    puts("network contract passed");
    return 0;
}
