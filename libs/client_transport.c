#include "client_transport.h"
#include <stdlib.h>
#include <string.h>

typedef struct { unsigned char data[PROTOCOL_FRAME_MAX]; size_t length; } Packet;
struct ClientTransport {
    Client *client;
    HANDLE thread;
    CRITICAL_SECTION lock;
    Packet commands[32];
    ClientEvent events[64];
    unsigned command_head, command_count, event_head, event_count;
    volatile LONG stop, done;
    int finish, error, registered;
    ProtocolDecoder decoder;
};

static int Emit(const ProtocolFrame *frame, void *argument)
{
    ClientTransport *t = argument;
    ClientEvent *event;
    EnterCriticalSection(&t->lock);
    /* Переполнение не теряет сообщение молча: сеанс завершается с ENOBUFS.
       Старые события остаются доступны основному потоку для показа/сохранения. */
    if (t->event_count == 64) { LeaveCriticalSection(&t->lock); t->error = WSAENOBUFS; return -1; }
    event = &t->events[(t->event_head + t->event_count++) % 64];
    event->type = frame->type; event->request = frame->request_id; event->length = frame->length;
    memcpy(event->payload, frame->payload, frame->length);
    LeaveCriticalSection(&t->lock);
    if (frame->type == PROTO_WELCOME) t->registered = 1;
    return 0;
}

static DWORD WINAPI Worker(LPVOID argument)
{
    ClientTransport *t = argument;
    DWORD started = GetTickCount(), partial = started, stalled = started, closing = 0;
    int half_closed = 0;
    while (!InterlockedCompareExchange(&t->stop, 0, 0)) {
        Packet packet;
        int has = 0, finish, ready, bytes;
        DWORD now = GetTickCount();
        fd_set read_set, write_set;
        struct timeval wait = {0, 20000};
        unsigned char buffer[4096];
        size_t before = t->client->tx_size;
        EnterCriticalSection(&t->lock);
        if (t->command_count && before + t->commands[t->command_head].length <= sizeof(t->client->tx_buffer)) {
            packet = t->commands[t->command_head]; t->command_head = (t->command_head + 1) % 32;
            --t->command_count; has = 1;
        }
        finish = t->finish && !t->command_count;
        LeaveCriticalSection(&t->lock);
        if (has) {
            if (Socket_Send(t->client, packet.data, (int)packet.length) < 0) { t->error = WSAGetLastError(); break; }
            if (!before) stalled = now;
        }
        if (t->client->tx_size) {
            int queued = t->client->tx_size;
            if (Socket_Flush(t->client) < 0) { t->error = WSAGetLastError(); break; }
            if (t->client->tx_size < queued) stalled = now;
            if (t->client->tx_size && now - stalled >= 5000) { t->error = WSAETIMEDOUT; break; }
        }
        if (finish && !t->client->tx_size && !half_closed) {
            if (Client_BeginDrain(t->client) < 0 || Client_ShutdownSend(t->client) < 0) { t->error = WSAGetLastError(); break; }
            half_closed = 1; closing = now;
        }
        if ((!t->registered && now - started >= PROTOCOL_TIMEOUT_MS)
            || (t->decoder.used && now - partial >= PROTOCOL_TIMEOUT_MS)
            || (half_closed && now - closing >= 5000)) { t->error = WSAETIMEDOUT; break; }
        FD_ZERO(&read_set); FD_SET(t->client->socket, &read_set);
        FD_ZERO(&write_set); if (t->client->tx_size) FD_SET(t->client->socket, &write_set);
        ready = select(0, &read_set, &write_set, NULL, &wait);
        if (ready == SOCKET_ERROR) { t->error = WSAGetLastError(); break; }
        if (!FD_ISSET(t->client->socket, &read_set)) continue;
        bytes = Socket_Recv(t->client, (char *)buffer, sizeof(buffer));
        if (bytes > 0) {
            unsigned long previous = t->decoder.frames_started;
            if (Protocol_Feed(&t->decoder, buffer, (size_t)bytes, Emit, t) < 0) {
                if (!t->error) t->error = WSAEPROTONOSUPPORT;
                break;
            }
            if (t->decoder.used && previous != t->decoder.frames_started) partial = GetTickCount();
        } else if (!bytes) {
            if (!t->registered || !Protocol_Finished(&t->decoder)) t->error = WSAEPROTONOSUPPORT;
            break;
        } else if (bytes != SOCKET_RECV_WOULD_BLOCK) { t->error = WSAGetLastError(); break; }
    }
    InterlockedExchange(&t->done, 1); return 0;
}

ClientTransport *Transport_Create(Client *client)
{
    ClientTransport *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->client = client; InitializeCriticalSection(&t->lock); Protocol_Init(&t->decoder);
    t->thread = CreateThread(NULL, 0, Worker, t, 0, NULL);
    if (!t->thread) { DeleteCriticalSection(&t->lock); free(t); return NULL; }
    return t;
}

int Transport_Post(ClientTransport *t, uint8_t type, uint32_t request, const void *payload, size_t length)
{
    Packet packet;
    packet.length = Protocol_Encode(packet.data, sizeof(packet.data), type, request, payload, length);
    if (!packet.length) return 0;
    EnterCriticalSection(&t->lock);
    if (t->finish || t->command_count == 32 || InterlockedCompareExchange(&t->done, 0, 0)) {
        LeaveCriticalSection(&t->lock); return 0;
    }
    t->commands[(t->command_head + t->command_count++) % 32] = packet;
    LeaveCriticalSection(&t->lock); return 1;
}

int Transport_Pop(ClientTransport *t, ClientEvent *event)
{
    EnterCriticalSection(&t->lock);
    if (!t->event_count) { LeaveCriticalSection(&t->lock); return 0; }
    *event = t->events[t->event_head]; t->event_head = (t->event_head + 1) % 64; --t->event_count;
    LeaveCriticalSection(&t->lock); return 1;
}

void Transport_Finish(ClientTransport *t)
{ EnterCriticalSection(&t->lock); t->finish = 1; LeaveCriticalSection(&t->lock); }
int Transport_Done(ClientTransport *t, int *error)
{
    if (!InterlockedCompareExchange(&t->done, 0, 0)) return 0;
    *error = t->error; return 1;
}
void Transport_Destroy(ClientTransport *t)
{
    if (!t) return;
    InterlockedExchange(&t->stop, 1);
    WaitForSingleObject(t->thread, INFINITE); CloseHandle(t->thread);
    DeleteCriticalSection(&t->lock); free(t);
}
