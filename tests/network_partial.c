/** @file network_partial.c
 * @brief Детерминированные короткие операции и ошибки системного ввода-вывода.
 */
#include "network.h"
#include "network_io_stub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
static int next_result, next_error, calls, wire_size;
static char wire[NETWORK_TX_CAPACITY], full[NETWORK_TX_CAPACITY];
static const char *incoming;

int NetworkTest_Send(SOCKET socket, const char *buffer, int length, int flags)
{
    (void)socket; (void)flags;
    ++calls;
    if (next_result == SOCKET_ERROR) { WSASetLastError(next_error); return SOCKET_ERROR; }
    CHECK(next_result <= length);
    CHECK(wire_size + next_result <= (int)sizeof(wire));
    memcpy(wire + wire_size, buffer, (size_t)next_result);
    wire_size += next_result;
    return next_result;
}

int NetworkTest_Recv(SOCKET socket, char *buffer, int length, int flags)
{
    (void)socket; (void)flags;
    ++calls;
    if (next_result == SOCKET_ERROR) { WSASetLastError(next_error); return SOCKET_ERROR; }
    CHECK(next_result <= length);
    if (next_result > 0) memcpy(buffer, incoming, (size_t)next_result);
    return next_result;
}

int main(void)
{
    Client client = CLIENT_INITIALIZER;
    char received[16];
    int before;
    CHECK(Network_Initialize() == 0);
    /* Дескриптор не принадлежит ОС: все send/recv этой цели подменены,
       закрывать такой маркер через closesocket запрещено. */
    client.socket = (SOCKET)123;
    client.state = CLIENT_OPEN;
    CHECK(Socket_Send(&client, "abcdef", 6) == 0);
    next_result = 3;
    CHECK(Socket_Flush(&client) == 1 && client.tx_size == 3);
    CHECK(memcmp(client.tx_buffer, "def", 3) == 0);
    CHECK(Socket_Send(&client, "ghi", 3) == 0);
    next_result = SOCKET_ERROR; next_error = WSAEWOULDBLOCK;
    CHECK(Socket_Flush(&client) == 1 && client.tx_size == 6);
    CHECK(Client_GetState(&client) == CLIENT_OPEN);
    CHECK(memcmp(client.tx_buffer, "defghi", 6) == 0);
    next_result = 2;
    CHECK(Socket_Flush(&client) == 1 && client.tx_size == 4);
    next_result = 4;
    CHECK(Socket_Flush(&client) == 0 && client.tx_size == 0);
    CHECK(wire_size == 9 && memcmp(wire, "abcdefghi", 9) == 0);
    before = calls;
    CHECK(Socket_Flush(&client) == 0 && calls == before);

    CHECK(Socket_Send(&client, full, sizeof(full)) == 0);
    CHECK(Socket_Send(&client, "x", 1) == SOCKET_QUEUE_FULL);
    CHECK(client.tx_size == NETWORK_TX_CAPACITY && memcmp(client.tx_buffer, full, sizeof(full)) == 0);
    CHECK(Socket_Send(&client, NULL, 1) == -1);
    CHECK(Socket_Send(&client, full, -1) == -1);
    client.tx_size = NETWORK_TX_CAPACITY + 1;
    CHECK(Socket_Flush(&client) == -1 && calls == before);
    client.tx_size = 0;

    incoming = "ab"; next_result = 2;
    memset(received, '!', sizeof(received));
    CHECK(Socket_Recv(&client, received, sizeof(received)) == 2);
    CHECK(received[2] == '!'); /* Нет неоговорённого NUL после фрагмента. */
    next_result = SOCKET_ERROR; next_error = WSAEWOULDBLOCK;
    CHECK(Socket_Recv(&client, received + 2, sizeof(received) - 2) == SOCKET_RECV_WOULD_BLOCK);
    incoming = "cde"; next_result = 3;
    CHECK(Socket_Recv(&client, received + 2, sizeof(received) - 2) == 3);
    CHECK(memcmp(received, "abcde", 5) == 0);
    next_result = 0;
    CHECK(Socket_Recv(&client, received, sizeof(received)) == 0 && Client_HasPeerEOF(&client));
    before = calls;
    CHECK(Socket_Recv(&client, received, sizeof(received)) == 0 && calls == before);
    CHECK(Socket_Send(&client, "reply", 5) == 0); /* EOF закрывает только RX. */
    next_result = SOCKET_ERROR; next_error = WSAECONNRESET;
    CHECK(Socket_Flush(&client) == -1 && WSAGetLastError() == WSAECONNRESET);
    CHECK(Client_GetState(&client) == CLIENT_FAILED && client.tx_size == 5);
    CHECK(Socket_Send(&client, "x", 1) == -1);
    client.state = CLIENT_OPEN; client.peer_eof = 0;
    next_error = WSAETIMEDOUT;
    CHECK(Socket_Recv(&client, received, sizeof(received)) == -1);
    CHECK(WSAGetLastError() == WSAETIMEDOUT && Client_GetState(&client) == CLIENT_FAILED);
    client.state = CLIENT_OPEN;
    next_result = 0;
    CHECK(Socket_Flush(&client) == -1 && Client_GetState(&client) == CLIENT_FAILED);
    client.socket = INVALID_SOCKET;
    Client_Close(&client);
    Network_Cleanup();
    puts("forced partial I/O passed");
    return 0;
}
