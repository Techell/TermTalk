/** @file network.c
 *  @brief Реализация соединения и ограниченной отправки байтов через Winsock.
 */
#include "network.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Только отдельная тестовая сборка подменяет системные операции. В приложениях
   нет изменяемых указателей на функции или доступного переключателя подмены. */
#ifdef WINSOCK_NETWORK_TESTING
#include "network_io_stub.h"
#define send NetworkTest_Send
#define recv NetworkTest_Recv
#endif

NetworkLocalError Network_ClassifyLocalError(int error)
{
    switch (error) {
    case WSAEINVAL: case WSAEFAULT: return NET_LOCAL_ARGUMENT;
    case WSAENOTSOCK: case WSAENOTCONN: case WSAESHUTDOWN:
    case WSAEISCONN: case WSAEALREADY: return NET_LOCAL_STATE;
    case WSAEADDRINUSE: return NET_LOCAL_ADDRESS_IN_USE;
    case WSAEADDRNOTAVAIL: return NET_LOCAL_ADDRESS_UNAVAILABLE;
    case WSAEACCES: return NET_LOCAL_ACCESS;
    case WSAENOBUFS: case WSAEMFILE: return NET_LOCAL_RESOURCES;
    case WSANOTINITIALISED: case WSAVERNOTSUPPORTED: return NET_LOCAL_NOT_INITIALIZED;
    default: return NET_LOCAL_NONE;
    }
}

NetworkError Network_ClassifyError(int error)
{
    if (Network_ClassifyLocalError(error) != NET_LOCAL_NONE) return NET_ERROR_LOCAL;
    switch (error) {
    case 0: return NET_ERROR_NONE;
    case WSAEWOULDBLOCK: return NET_ERROR_WAIT;
    case WSAECONNREFUSED: return NET_ERROR_REFUSED;
    case WSAENETUNREACH: case WSAEHOSTUNREACH: return NET_ERROR_UNREACHABLE;
    case WSAETIMEDOUT: return NET_ERROR_TIMEOUT;
    case WSAECONNRESET: case WSAECONNABORTED: case WSAENETRESET:
        return NET_ERROR_DISCONNECTED;
    default: return NET_ERROR_SYSTEM;
    }
}

const char *Network_ErrorText(int error)
{
    switch (Network_ClassifyLocalError(error)) {
    case NET_LOCAL_ARGUMENT: return "invalid argument";
    case NET_LOCAL_STATE: return "invalid socket state";
    case NET_LOCAL_ADDRESS_IN_USE: return "address already in use";
    case NET_LOCAL_ADDRESS_UNAVAILABLE: return "local address unavailable";
    case NET_LOCAL_ACCESS: return "access denied";
    case NET_LOCAL_RESOURCES: return "system resources exhausted";
    case NET_LOCAL_NOT_INITIALIZED: return "Winsock initialization required or unsupported version";
    default: break;
    }
    switch (Network_ClassifyError(error)) {
    case NET_ERROR_NONE: return "success";
    case NET_ERROR_WAIT: return "wait for socket readiness";
    case NET_ERROR_REFUSED: return "connection refused";
    case NET_ERROR_UNREACHABLE: return "network or host unreachable";
    case NET_ERROR_TIMEOUT: return "connection timed out";
    case NET_ERROR_DISCONNECTED: return "connection interrupted";
    default: return "Winsock failure";
    }
}

ClientState Client_GetState(Client *client)
{
    return client == NULL ? CLIENT_CLOSED
        : (ClientState)InterlockedCompareExchange(&client->state, 0, 0);
}

int Client_HasPeerEOF(Client *client)
{
    return client != NULL && InterlockedCompareExchange(&client->peer_eof, 0, 0) != 0;
}

static int Client_Fail(Client *client, int error)
{
    /* Ошибка запрещает новые операции, но не отнимает сокет у владельца.
       Другой поток может ещё завершать уже начатую операцию. */
    InterlockedExchange(&client->state, CLIENT_FAILED);
    WSASetLastError(error);
    return -1;
}

int Client_BeginDrain(Client *client)
{
    LONG previous;
    if (client == NULL) { WSASetLastError(WSAEINVAL); return -1; }
    previous = InterlockedCompareExchange(&client->state, CLIENT_DRAINING, CLIENT_OPEN);
    if (previous != CLIENT_OPEN && previous != CLIENT_DRAINING) {
        WSASetLastError(WSAENOTCONN);
        return -1;
    }
    return 0;
}

int Client_ShutdownSend(Client *client)
{
    if (client == NULL) { WSASetLastError(WSAEINVAL); return -1; }
    if (Client_GetState(client) == CLIENT_SEND_CLOSED) return 0;
    if (Client_GetState(client) != CLIENT_DRAINING || client->tx_size != 0) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    if (shutdown(client->socket, SD_SEND) == SOCKET_ERROR)
        return Client_Fail(client, WSAGetLastError());
    /* Не затираем FAILED, установленное потоком приёма во время shutdown. */
    if (InterlockedCompareExchange(&client->state, CLIENT_SEND_CLOSED,
                                   CLIENT_DRAINING) != CLIENT_DRAINING) {
        WSASetLastError(WSAENOTCONN);
        return -1;
    }
    return 0;
}

//Переводим сокет клиента в исходное нулевое состояние
static void Client_Reset(Client *client)
{
    memset(client, 0, sizeof(*client));
    client->socket = INVALID_SOCKET;
}

static int Client_SetNonblocking(Client *client)
{
    u_long mode = 1;
    return ioctlsocket(client->socket, FIONBIO, &mode);
}

int Network_Initialize(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa);
}

void Network_Cleanup(void)
{
    WSACleanup();
}

void Client_Close(Client *client)
{
    if (client != NULL) {
        if (client->socket != INVALID_SOCKET) {
            closesocket(client->socket);
        }
        Client_Reset(client);
    }
}

int Client_Initialize(Client *client, const char *ip_server, u_short port_server)
{ return Client_ConnectTimed(client, ip_server, port_server, 3000, NULL); }

int Client_ConnectTimed(Client *client, const char *ip_server, u_short port_server, unsigned timeout_ms, HANDLE cancel)
{
    struct sockaddr_in address = {0};
    int error;
    if (client == NULL || ip_server == NULL || port_server == 0 || !timeout_ms || timeout_ms > 30000) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    if (client->socket != INVALID_SOCKET || Client_GetState(client) != CLIENT_CLOSED) {
        WSASetLastError(WSAEISCONN);
        return -1;
    }
    Client_Reset(client);
    address.sin_family = AF_INET;
    address.sin_port = htons(port_server);
    /* Available in the existing MinGW SDK. */
    {
        char text[MAX_IP_SIZE];
        int size = sizeof(address);
        if (strlen(ip_server) >= sizeof(text)) {
            WSASetLastError(WSAEINVAL);
            return -1;
        }
        strcpy(text, ip_server);
        if (WSAStringToAddressA(text, AF_INET, NULL,
                               (struct sockaddr *)&address, &size) != 0) {
            return -1;
        }
        address.sin_port = htons(port_server);
    }
    client->socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client->socket == INVALID_SOCKET) {
        return -1;
    }
    InterlockedExchange(&client->state, CLIENT_CONNECTING);
    if (Client_SetNonblocking(client) == SOCKET_ERROR) {
        /* Очистка ресурса не должна скрыть причину исходной ошибки. */
        error = WSAGetLastError();
        Client_Close(client);
        WSASetLastError(error);
        return -1;
    }
    if (connect(client->socket, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
        DWORD started = GetTickCount();
        error = WSAGetLastError();
        if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS) goto connect_failed;
        while (1) {
            fd_set writable, exceptional;
            struct timeval wait = {0, 20000};
            int ready, size = sizeof(error);
            if (cancel && WaitForSingleObject(cancel, 0) != WAIT_TIMEOUT) { error = WSAEINTR; goto connect_failed; }
            if (GetTickCount() - started >= timeout_ms) { error = WSAETIMEDOUT; goto connect_failed; }
            FD_ZERO(&writable); FD_SET(client->socket, &writable);
            FD_ZERO(&exceptional); FD_SET(client->socket, &exceptional);
            ready = select(0, NULL, &writable, &exceptional, &wait);
            if (ready == SOCKET_ERROR) { error = WSAGetLastError(); goto connect_failed; }
            if (!ready) continue;
            if (getsockopt(client->socket, SOL_SOCKET, SO_ERROR, (char *)&error, &size) == SOCKET_ERROR) error = WSAGetLastError();
            if (error) goto connect_failed;
            break;
        }
    }
    InterlockedExchange(&client->state, CLIENT_OPEN);
    strcpy(client->ip_connected, ip_server);
    client->port_connected = port_server;
    return 0;
connect_failed:
    Client_Close(client); WSASetLastError(error); return -1;
}

int Server_Listen(SOCKET *listener, u_short port)
{
    struct sockaddr_in address = {0};
    SOCKET created;
    BOOL exclusive = TRUE;
    u_long nonblocking = 1;
    int error;
    if (listener == NULL) { WSASetLastError(WSAEINVAL); return -1; }
    if (*listener != INVALID_SOCKET) { WSASetLastError(WSAEISCONN); return -1; }
    created = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (created == INVALID_SOCKET) return -1;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(port);
    if (setsockopt(created, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   (const char *)&exclusive, sizeof(exclusive)) == SOCKET_ERROR
        || bind(created, (struct sockaddr *)&address, sizeof(address)) == SOCKET_ERROR
        || listen(created, SOMAXCONN) == SOCKET_ERROR
        || ioctlsocket(created, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        error = WSAGetLastError();
        closesocket(created);
        WSASetLastError(error);
        return -1;
    }
    /* Передаём владение только полностью настроенным слушающим сокетом. */
    *listener = created;
    return 0;
}

int Server_Accept(SOCKET listener, Client *client)
{
    struct sockaddr_in peer = {0};
    int size = sizeof(peer), error;
    SOCKET accepted;
    if (client == NULL || listener == INVALID_SOCKET) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    if (client->socket != INVALID_SOCKET || Client_GetState(client) != CLIENT_CLOSED) {
        WSASetLastError(WSAEISCONN);
        return -1;
    }
    accepted = accept(listener, (struct sockaddr *)&peer, &size);
    if (accepted == INVALID_SOCKET)
        return WSAGetLastError() == WSAEWOULDBLOCK ? 1 : -1;
    client->socket = accepted;
    if (Client_SetNonblocking(client) == SOCKET_ERROR) {
        error = WSAGetLastError();
        Client_Close(client);
        WSASetLastError(error);
        return -1;
    }
    strcpy(client->ip_connected, inet_ntoa(peer.sin_addr));
    client->port_connected = ntohs(peer.sin_port);
    InterlockedExchange(&client->state, CLIENT_OPEN);
    return 0;
}

int Socket_Flush(Client *client)
{
    int sent;
    if (client == NULL || client->socket == INVALID_SOCKET
        || client->tx_size < 0 || client->tx_size > NETWORK_TX_CAPACITY) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    if (Client_GetState(client) != CLIENT_OPEN && Client_GetState(client) != CLIENT_DRAINING) {
        WSASetLastError(WSAENOTCONN);
        return -1;
    }
    if (client->tx_size == 0) {
        return 0;
    }
    sent = send(client->socket, client->tx_buffer, client->tx_size, 0);
    if (sent == SOCKET_ERROR) {
        int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK ? 1 : Client_Fail(client, error);
    }
    if (sent == 0) {
        return Client_Fail(client, WSAECONNRESET);
    }
    client->tx_size -= sent;
    if (client->tx_size > 0) {
        /* Области перекрываются: сдвигаем остаток на число отправленных байтов. */
        memmove(client->tx_buffer, client->tx_buffer + sent, (size_t)client->tx_size);
    }
    return client->tx_size > 0 ? 1 : 0;
}

int Socket_Send(Client *client, const void *data, int length)
{
    if (client == NULL || client->socket == INVALID_SOCKET || length < 0
        || (data == NULL && length > 0)
        || client->tx_size < 0 || client->tx_size > NETWORK_TX_CAPACITY) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    if (Client_GetState(client) != CLIENT_OPEN) {
        WSASetLastError(WSAENOTCONN);
        return -1;
    }
    if (length > NETWORK_TX_CAPACITY - client->tx_size) return SOCKET_QUEUE_FULL;
    if (length > 0) {
        memcpy(client->tx_buffer + client->tx_size, data, (size_t)length);
        client->tx_size += length;
    }
    return 0;
}

int Socket_Recv(Client *client, char *buffer, int buffer_size)
{
    int bytes;
    ClientState state;
    if (client == NULL || client->socket == INVALID_SOCKET || buffer == NULL || buffer_size <= 0) {
        WSASetLastError(WSAEINVAL);
        return -1;
    }
    state = Client_GetState(client);
    if (state != CLIENT_OPEN && state != CLIENT_DRAINING && state != CLIENT_SEND_CLOSED) {
        WSASetLastError(WSAENOTCONN);
        return -1;
    }
    if (Client_HasPeerEOF(client)) return 0;
    bytes = recv(client->socket, buffer, buffer_size, 0);
    if (bytes == SOCKET_ERROR) {
        int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK ? SOCKET_RECV_WOULD_BLOCK : Client_Fail(client, error);
    }
    if (bytes == 0) InterlockedExchange(&client->peer_eof, 1);
    return bytes;
}

int Parse_Port(const char *text, u_short *port)
{
    char *end;
    unsigned long value;
    const char *p;
    if (text == NULL || port == NULL || *text == '\0') {
        return -1;
    }
    for (p = text; *p != '\0'; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
    }
    errno = 0;
    // Преобразование в десятичное число из строки
    value = strtoul(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || value == 0 || value > 65535) {
        return -1;
    }
    *port = (u_short)value;
    return 0;
}


