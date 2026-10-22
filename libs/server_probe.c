#include "server_probe.h"
#include "protocol.h"
#include <stdlib.h>

struct ServerProbe { HANDLE thread, cancel; volatile LONG done; ServerProbeResult result; int received; };
static int Response(const ProtocolFrame *frame, void *context)
{
    ServerProbe *p = context;
    if (frame->type != PROTO_STATUS_RESPONSE || frame->request_id != 1 || p->received) return -1;
    p->result.users = Protocol_Read16(frame->payload); p->result.capacity = Protocol_Read16(frame->payload + 2);
    p->result.file_limit = Protocol_Read64(frame->payload + 4); p->received = 1; return 0;
}
static DWORD WINAPI Probe(LPVOID context)
{
    ServerProbe *p = context;
    Client client = CLIENT_INITIALIZER;
    ProtocolDecoder decoder;
    unsigned char bytes[512];
    DWORD began = GetTickCount(), sent;
    int network = Network_Initialize();
    p->result.error = network;
    if (network) goto finish;
    if (Client_ConnectTimed(&client, p->result.address.ip, p->result.address.port, 1500, p->cancel)) { p->result.error = WSAGetLastError(); goto cleanup; }
    Protocol_Init(&decoder);
    if (Socket_Send(&client, bytes, (int)Protocol_Encode(bytes, sizeof(bytes), PROTO_STATUS_REQUEST, 1, NULL, 0))) { p->result.error = WSAGetLastError(); goto cleanup; }
    sent = GetTickCount(); p->result.error = WSAETIMEDOUT;
    while (GetTickCount() - began < 1500 && WaitForSingleObject(p->cancel, 0) == WAIT_TIMEOUT) {
        fd_set readable, writable;
        struct timeval wait = {0, 20000};
        int ready, n;
        if (Socket_Flush(&client) < 0) { p->result.error = WSAGetLastError(); break; }
        FD_ZERO(&readable); FD_SET(client.socket, &readable);
        FD_ZERO(&writable); if (client.tx_size) FD_SET(client.socket, &writable);
        ready = select(0, &readable, &writable, NULL, &wait);
        if (ready == SOCKET_ERROR) { p->result.error = WSAGetLastError(); break; }
        if (!FD_ISSET(client.socket, &readable)) continue;
        n = Socket_Recv(&client, (char *)bytes, sizeof(bytes));
        if (n == SOCKET_RECV_WOULD_BLOCK) continue;
        if (n <= 0) { p->result.error = n == 0 ? WSAECONNRESET : WSAGetLastError(); break; }
        if (Protocol_Feed(&decoder, bytes, (size_t)n, Response, p) < 0) { p->result.error = WSAEPROTONOSUPPORT; break; }
        if (p->received && Protocol_Finished(&decoder)) {
            p->result.online = 1; p->result.latency = GetTickCount() - sent; p->result.error = 0; break;
        }
    }
cleanup:
    Client_Close(&client); Network_Cleanup();
finish:
    InterlockedExchange(&p->done, 1); return 0;
}
ServerProbe *ServerProbe_Start(const ServerAddress *address)
{
    ServerProbe *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->result.address = *address; p->cancel = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (p->cancel) p->thread = CreateThread(NULL, 0, Probe, p, 0, NULL);
    if (!p->thread) { if (p->cancel) CloseHandle(p->cancel); free(p); return NULL; }
    return p;
}
int ServerProbe_Read(ServerProbe *probe, ServerProbeResult *result)
{
    if (!InterlockedCompareExchange(&probe->done, 0, 0)) return 0;
    *result = probe->result; return 1;
}
void ServerProbe_Destroy(ServerProbe *probe)
{
    if (!probe) return;
    SetEvent(probe->cancel); WaitForSingleObject(probe->thread, INFINITE);
    CloseHandle(probe->thread); CloseHandle(probe->cancel); free(probe);
}
