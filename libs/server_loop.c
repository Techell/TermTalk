/** @file server_loop.c
 * @brief Справедливое обслуживание ограниченного реестра неблокирующих сокетов.
 */
#include "server_loop.h"
#include <stdlib.h>

#if SERVER_CLIENT_CAPACITY + 1 > FD_SETSIZE
#error SERVER_CLIENT_CAPACITY exceeds Winsock fd_set capacity
#endif

typedef struct {
    Client client;
    DWORD last_progress;
    DWORD drain_started;
    int had_pending;
    int was_draining;
} ServerSlot;

struct ServerLoop {
    SOCKET listener;
    u_short port;
    unsigned cursor;
    ServerLoopStats stats;
    ServerLifecycleHandler lifecycle;
    void *lifecycle_context;
    unsigned limit;
    ServerSlot slots[SERVER_CLIENT_CAPACITY];
};

static void Slot_Close(ServerLoop *loop, ServerSlot *slot)
{
    if (slot->client.socket != INVALID_SOCKET) {
        if (loop->lifecycle != NULL)
            loop->lifecycle(&slot->client, SERVER_CONNECTION_CLOSE, loop->lifecycle_context);
        Client_Close(&slot->client);
        --loop->stats.active;
    }
}

int ServerLoop_SetLifecycle(ServerLoop *loop, ServerLifecycleHandler handler, void *context)
{
    if (loop == NULL || loop->stats.active != 0) { WSASetLastError(WSAEINVAL); return -1; }
    loop->lifecycle = handler;
    loop->lifecycle_context = context;
    return 0;
}

int ServerLoop_Disconnect(ServerLoop *loop, Client *client)
{
    unsigned i;
    if (loop != NULL && client != NULL)
        for (i = 0; i < loop->limit; ++i)
            if (&loop->slots[i].client == client && client->socket != INVALID_SOCKET) {
                Slot_Close(loop, &loop->slots[i]);
                return 0;
            }
    WSASetLastError(WSAEINVAL);
    return -1;
}

ServerLoop *ServerLoop_Create(u_short port)
{ return ServerLoop_CreateConfigured(port, SERVER_CLIENT_LIMIT); }

ServerLoop *ServerLoop_CreateConfigured(u_short port, unsigned limit)
{
    ServerLoop *loop;
    struct sockaddr_in address;
    int size = sizeof(address), error;
    unsigned i;
    if (!limit || limit > SERVER_CLIENT_CAPACITY) { WSASetLastError(WSAEINVAL); return NULL; }
    loop = calloc(1, sizeof(*loop));
    if (loop == NULL) { WSASetLastError(WSAENOBUFS); return NULL; }
    loop->limit = limit;
    loop->listener = INVALID_SOCKET;
    for (i = 0; i < loop->limit; ++i)
        loop->slots[i].client.socket = INVALID_SOCKET;
    if (Server_Listen(&loop->listener, port) < 0
        || getsockname(loop->listener, (struct sockaddr *)&address, &size) == SOCKET_ERROR) {
        error = WSAGetLastError();
        ServerLoop_Destroy(loop);
        WSASetLastError(error);
        return NULL;
    }
    loop->port = ntohs(address.sin_port);
    return loop;
}

void ServerLoop_Destroy(ServerLoop *loop)
{
    unsigned i;
    if (loop == NULL) return;
    for (i = 0; i < loop->limit; ++i) Slot_Close(loop, &loop->slots[i]);
    if (loop->listener != INVALID_SOCKET) closesocket(loop->listener);
    free(loop);
}

u_short ServerLoop_Port(const ServerLoop *loop) { return loop == NULL ? 0 : loop->port; }

ServerLoopStats ServerLoop_GetStats(const ServerLoop *loop)
{
    ServerLoopStats empty = {0};
    return loop == NULL ? empty : loop->stats;
}

int ServerLoop_Broadcast(ServerLoop *loop, const Client *sender, const void *data, int length)
{
    unsigned i;
    int queued = 0;
    DWORD now = GetTickCount();
    if (loop == NULL || length < 0 || length > NETWORK_TX_CAPACITY
        || (data == NULL && length != 0)) { WSASetLastError(WSAEINVAL); return -1; }
    for (i = 0; i < loop->limit; ++i) {
        ServerSlot *slot = &loop->slots[i];
        Client *client = &slot->client;
        int was_empty, result;
        if (client == sender || Client_GetState(client) != CLIENT_OPEN) continue;
        was_empty = client->tx_size == 0;
        result = Socket_Send(client, data, length);
        if (result == SOCKET_QUEUE_FULL) {
            /* Отбрасывать произвольный фрагмент TCP-потока нельзя: последующий
               кодек получил бы повреждённый поток. Закрываем только получателя. */
            ++loop->stats.slow;
            Slot_Close(loop, slot);
        } else if (result < 0) {
            Slot_Close(loop, slot);
        } else {
            if (was_empty && length != 0) slot->last_progress = now;
            ++queued;
        }
    }
    return queued;
}

static int Accept_Ready(ServerLoop *loop)
{
    unsigned attempt;
    for (attempt = 0; attempt < loop->limit; ++attempt) {
        unsigned i;
        int result;
        Client rejected = CLIENT_INITIALIZER;
        ServerSlot *free_slot = NULL;
        for (i = 0; i < loop->limit; ++i)
            if (loop->slots[i].client.socket == INVALID_SOCKET) { free_slot = &loop->slots[i]; break; }
        /* Даже при полном реестре снимаем accept из backlog и закрываем лишний
           сокет, чтобы listener не будил select бесконечно без полезной работы. */
        result = Server_Accept(loop->listener, free_slot == NULL ? &rejected : &free_slot->client);
        if (result == 1) return 0;
        if (result < 0) {
            int error = WSAGetLastError();
            if (error == WSAECONNRESET || error == WSAECONNABORTED) continue;
            return -1;
        }
        if (free_slot == NULL) {
            ++loop->stats.rejected;
            Client_Close(&rejected);
        } else {
            free_slot->last_progress = GetTickCount();
            free_slot->drain_started = 0;
            free_slot->had_pending = 0;
            free_slot->was_draining = 0;
            ++loop->stats.active;
            ++loop->stats.accepted;
            if (loop->lifecycle != NULL)
                loop->lifecycle(&free_slot->client, SERVER_CONNECTION_OPEN, loop->lifecycle_context);
        }
    }
    return 0;
}

int ServerLoop_Poll(ServerLoop *loop, int timeout_ms, ServerDataHandler handler, void *context)
{
    fd_set readable, writable;
    struct timeval timeout;
    unsigned n;
    int ready;
    DWORD now = GetTickCount();
    if (loop == NULL || handler == NULL || timeout_ms < 1 || timeout_ms > 1000) {
        WSASetLastError(WSAEINVAL); return -1;
    }
    FD_ZERO(&readable);
    FD_ZERO(&writable);
    FD_SET(loop->listener, &readable);
    for (n = 0; n < loop->limit; ++n) {
        ServerSlot *slot = &loop->slots[n];
        Client *client = &slot->client;
        ClientState state = Client_GetState(client);
        if (state == CLIENT_CLOSED) continue;
        /* Обработчик может поставить ответ через Socket_Send напрямую. Начало
           непустой очереди отслеживаем и здесь, а не только в Broadcast. */
        if (client->tx_size > 0 && !slot->had_pending) slot->last_progress = now;
        slot->had_pending = client->tx_size > 0;
        /* Прикладной callback также может начать drain, не дожидаясь EOF.
           Его срок начинается при переходе, а не со времени старта процесса. */
        if (state == CLIENT_DRAINING && !slot->was_draining) slot->drain_started = now;
        slot->was_draining = state == CLIENT_DRAINING;
        /* Вычитание DWORD сохраняет корректность коротких интервалов при
           переполнении GetTickCount. Таймеры ограничивают жизнь зависшего TX. */
        if (state == CLIENT_FAILED || (state == CLIENT_DRAINING
            && (client->tx_size == 0 || now - slot->drain_started >= SERVER_DRAIN_TIMEOUT_MS))) {
            Slot_Close(loop, slot);
            continue;
        }
        if (client->tx_size > 0 && now - slot->last_progress >= SERVER_STALL_TIMEOUT_MS) {
            ++loop->stats.slow;
            Slot_Close(loop, slot);
            continue;
        }
        if (state == CLIENT_OPEN) FD_SET(client->socket, &readable);
        if (client->tx_size > 0) FD_SET(client->socket, &writable);
    }
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;
    ++loop->stats.waits;
    ready = select(0, &readable, &writable, NULL, &timeout);
    if (ready == SOCKET_ERROR) return -1;
    if (ready == 0) return 0;

    /* Сначала освобождаем готовые TX: активный отправитель не должен заполнять
       очереди остальных раньше, чем они получат свою возможность отправки. */
    for (n = 0; n < loop->limit; ++n) {
        ServerSlot *slot = &loop->slots[(loop->cursor + n) % loop->limit];
        Client *client = &slot->client;
        int previous = client->tx_size;
        if (client->socket == INVALID_SOCKET || !FD_ISSET(client->socket, &writable)) continue;
        if (Socket_Flush(client) < 0) Slot_Close(loop, slot);
        else if (client->tx_size < previous) {
            slot->last_progress = GetTickCount();
            slot->had_pending = client->tx_size > 0;
        }
    }
    for (n = 0; n < loop->limit; ++n) {
        ServerSlot *slot = &loop->slots[(loop->cursor + n) % loop->limit];
        Client *client = &slot->client;
        char data[INPUT_BUF_MAX];
        int length;
        if (Client_GetState(client) != CLIENT_OPEN || !FD_ISSET(client->socket, &readable)) continue;
        length = Socket_Recv(client, data, sizeof(data));
        if (length > 0) {
            if (handler(loop, client, data, length, context) < 0) Slot_Close(loop, slot);
        } else if (length == 0) {
            Client_BeginDrain(client);
            slot->drain_started = GetTickCount();
            slot->was_draining = 1;
            if (client->tx_size == 0) Slot_Close(loop, slot);
        } else if (length != SOCKET_RECV_WOULD_BLOCK) Slot_Close(loop, slot);
    }
    loop->cursor = (loop->cursor + 1) % loop->limit;
    /* Новые дескрипторы принимаем после обслуживания снимка fd_set: Windows
       может переиспользовать закрытый HANDLE, которому остались старые флаги. */
    if (FD_ISSET(loop->listener, &readable)) return Accept_Ready(loop);
    return 0;
}
