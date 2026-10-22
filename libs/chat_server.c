/** @file chat_server.c
 * @brief Прикладные состояния отделены от ClientState транспорта.
 */
#include "chat_server.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    ChatServer *server;
    Client *connection; /* Заимствование строго между lifecycle OPEN/CLOSE. */
    ProtocolDecoder decoder;
    uint32_t id, last_request;
    char nickname[PROTOCOL_NICK_MAX + 1];
    DWORD connected_at, frame_started_at;
    int active, closing;
} ChatPeer;

typedef struct {
    ChatPeer *sender, *recipient;
    uint32_t id;
    uint64_t total, committed, pending;
    DWORD progress_at;
    int state; /* 1 предложение, 2 данные, 3 проверка/сохранение получателя. */
} FileRoute;

struct ChatServer {
    ServerLoop *loop;
    ChatPeer peers[SERVER_CLIENT_CAPACITY];
    uint32_t next_id;
    int roster_dirty;
    unsigned max_users;
    uint64_t max_file_bytes;
    FileRoute files[SERVER_CLIENT_CAPACITY / 2];
    uint32_t next_transfer;
};

static void Drop_Files(ChatPeer *peer, unsigned char reason);

static ChatPeer *Find_Connection(ChatServer *server, Client *connection)
{
    unsigned i;
    for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i)
        if (server->peers[i].connection == connection) return &server->peers[i];
    return NULL;
}

static int Peer_Active(ChatPeer *peer)
{
    return peer->connection != NULL && peer->active && !peer->closing
        && Client_GetState(peer->connection) == CLIENT_OPEN;
}

static int Queue_Frame(ChatPeer *peer, uint8_t type, uint32_t request, const void *data, size_t length)
{
    unsigned char frame[PROTOCOL_FRAME_MAX];
    size_t n = Protocol_Encode(frame, sizeof(frame), type, request, data, length);
    if (n == 0) return -1;
    return Socket_Send(peer->connection, frame, (int)n);
}

static void Begin_Close(ChatPeer *peer)
{
    Drop_Files(peer, 5);
    peer->closing = 1;
    if (peer->active) peer->server->roster_dirty = 1;
    peer->active = 0;
    Client_BeginDrain(peer->connection);
}

static int Reply_Error(ChatPeer *peer, uint32_t request, ProtocolError code, const char *detail, int fatal)
{
    unsigned char payload[132];
    size_t n = strlen(detail);
    if (n > 128) n = 128;
    Protocol_Write16(payload, (uint16_t)code); Protocol_Write16(payload + 2, (uint16_t)n);
    memcpy(payload + 4, detail, n);
    /* Если даже ERROR не помещается, поток нельзя продолжать частичным ответом. */
    if (Queue_Frame(peer, PROTO_ERROR, request, payload, n + 4) != 0) return -1;
    if (fatal) Begin_Close(peer);
    return 0;
}

static size_t Build_Roster(ChatServer *server, unsigned char *payload)
{
    unsigned i, count = 0;
    size_t at = 2;
    for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i) {
        ChatPeer *peer = &server->peers[i];
        size_t nick;
        u_long address;
        if (!Peer_Active(peer)) continue;
        nick = strlen(peer->nickname);
        Protocol_Write32(payload + at, peer->id); payload[at + 4] = 1;
        Protocol_Write16(payload + at + 5, (uint16_t)nick); at += 7;
        memcpy(payload + at, peer->nickname, nick); at += nick;
        address = inet_addr(peer->connection->ip_connected);
        memcpy(payload + at, &address, 4);
        Protocol_Write16(payload + at + 4, peer->connection->port_connected); at += 6;
        ++count;
    }
    Protocol_Write16(payload, (uint16_t)count);
    return at;
}

static unsigned Active_Count(ChatServer *server)
{
    unsigned i, count = 0;
    for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i) if (Peer_Active(&server->peers[i])) ++count;
    return count;
}

static int Send_Status(ChatPeer *peer, uint32_t request)
{
    unsigned char payload[12];
    Protocol_Write16(payload, (uint16_t)Active_Count(peer->server));
    Protocol_Write16(payload + 2, (uint16_t)peer->server->max_users);
    Protocol_Write64(payload + 4, peer->server->max_file_bytes);
    return Queue_Frame(peer, PROTO_STATUS_RESPONSE, request, payload, sizeof(payload));
}

static int Send_Roster(ChatPeer *peer, uint32_t request)
{
    unsigned char payload[2 + PROTOCOL_USERS_MAX * (13 + PROTOCOL_NICK_MAX)];
    size_t n = Build_Roster(peer->server, payload);
    return Queue_Frame(peer, PROTO_LIST_RESPONSE, request, payload, n);
}

static void Cancel_Route(FileRoute *file, unsigned char reason)
{
    unsigned char payload[5];
    ChatPeer *sender = file->sender, *recipient = file->recipient;
    if (!file->state) return;
    Protocol_Write32(payload, file->id); payload[4] = reason;
    /* Сначала освобождаем маршрут: уведомление о переполнении может закрыть
       один из сеансов и повторно запустить очистку его передач. */
    memset(file, 0, sizeof(*file));
    if (Peer_Active(sender) && Queue_Frame(sender, PROTO_FILE_CANCEL, 0, payload, 5)) Begin_Close(sender);
    if (Peer_Active(recipient) && Queue_Frame(recipient, PROTO_FILE_CANCEL, 0, payload, 5)) Begin_Close(recipient);
}

static void Drop_Files(ChatPeer *peer, unsigned char reason)
{
    unsigned i;
    for (i = 0; i < SERVER_CLIENT_CAPACITY / 2; ++i) {
        FileRoute *file = &peer->server->files[i];
        if (file->state && (file->sender == peer || file->recipient == peer)) Cancel_Route(file, reason);
    }
}

static int File_Frame(ChatPeer *peer, const ProtocolFrame *frame)
{
    ChatServer *server = peer->server;
    const unsigned char *p = frame->payload;
    FileRoute *file = NULL;
    unsigned i;
    if (frame->type == PROTO_FILE_OFFER) {
        ChatPeer *recipient = NULL;
        unsigned char event[18 + PROTOCOL_FILE_NAME_MAX], created[8];
        for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i)
            if (Peer_Active(&server->peers[i]) && server->peers[i].id == Protocol_Read32(p)) recipient = &server->peers[i];
        if (!server->max_file_bytes || Protocol_Read64(p + 4) > server->max_file_bytes)
            return Reply_Error(peer, frame->request_id, PROTO_BAD_TRANSFER, "file exceeds server limit", 0);
        if (!recipient || recipient == peer) return Reply_Error(peer, frame->request_id, PROTO_RECIPIENT_UNAVAILABLE, "recipient unavailable", 0);
        for (i = 0; i < SERVER_CLIENT_CAPACITY / 2; ++i) {
            FileRoute *candidate = &server->files[i];
            if (!candidate->state) file = candidate;
            else if (candidate->sender == peer || candidate->recipient == peer || candidate->sender == recipient || candidate->recipient == recipient)
                return Reply_Error(peer, frame->request_id, PROTO_BAD_TRANSFER, "participant already has a transfer", 0);
        }
        if (!file || !server->next_transfer) return Reply_Error(peer, frame->request_id, PROTO_BAD_TRANSFER, "transfer limit", 0);
        if (NETWORK_TX_CAPACITY - peer->connection->tx_size < PROTOCOL_HEADER_SIZE + 8
            || NETWORK_TX_CAPACITY - recipient->connection->tx_size < (int)(PROTOCOL_HEADER_SIZE + frame->length + 4))
            return Reply_Error(peer, frame->request_id, PROTO_QUEUE_FULL, "offer was not queued", 0);
        Protocol_Write32(created, server->next_transfer); Protocol_Write32(created + 4, recipient->id);
        Protocol_Write32(event, server->next_transfer); Protocol_Write32(event + 4, peer->id);
        memcpy(event + 8, p + 4, frame->length - 4);
        if (Queue_Frame(peer, PROTO_FILE_CREATED, frame->request_id, created, 8)
            || Queue_Frame(recipient, PROTO_FILE_OFFER_EVENT, 0, event, frame->length + 4)) return -1;
        file->id = server->next_transfer++; file->sender = peer; file->recipient = recipient;
        file->state = 1; file->total = Protocol_Read64(p + 4); file->progress_at = GetTickCount(); return 0;
    }
    for (i = 0; i < SERVER_CLIENT_CAPACITY / 2; ++i)
        if (server->files[i].state && server->files[i].id == Protocol_Read32(p)) file = &server->files[i];
    if (!file || (file->sender != peer && file->recipient != peer))
        return Reply_Error(peer, frame->request_id, PROTO_BAD_TRANSFER, "unknown transfer or wrong role", 0);
    if (frame->type == PROTO_FILE_CANCEL) { Cancel_Route(file, p[4]); return 0; }
    if (frame->type == PROTO_FILE_DECISION && file->recipient == peer && file->state == 1) {
        if (Queue_Frame(file->sender, PROTO_FILE_DECISION_EVENT, 0, p, 5)) { Cancel_Route(file, 3); return 0; }
        if (!p[4]) memset(file, 0, sizeof(*file));
        else { file->state = 2; file->progress_at = GetTickCount(); }
        return 0;
    }
    if (frame->type == PROTO_FILE_DATA && file->sender == peer && file->state == 2
        && file->pending == file->committed && Protocol_Read64(p + 4) == file->committed
        && frame->length - 12 <= file->total - file->committed) {
        if (Queue_Frame(file->recipient, PROTO_FILE_DATA, 0, p, frame->length)) { Cancel_Route(file, 3); return 0; }
        file->pending = file->committed + frame->length - 12; file->progress_at = GetTickCount(); return 0;
    }
    if (frame->type == PROTO_FILE_ACK && file->recipient == peer && file->state == 2
        && file->pending > file->committed && Protocol_Read64(p + 4) == file->pending) {
        if (Queue_Frame(file->sender, PROTO_FILE_ACK, 0, p, 12)) { Cancel_Route(file, 3); return 0; }
        file->committed = file->pending; file->progress_at = GetTickCount(); return 0;
    }
    if (frame->type == PROTO_FILE_END && file->sender == peer && file->state == 2
        && file->committed == file->total && file->pending == file->committed) {
        if (Queue_Frame(file->recipient, PROTO_FILE_END, 0, p, 36)) { Cancel_Route(file, 3); return 0; }
        file->state = 3; file->progress_at = GetTickCount(); return 0;
    }
    if (frame->type == PROTO_FILE_DONE && file->recipient == peer && file->state == 3) {
        if (Queue_Frame(file->sender, PROTO_FILE_DONE, 0, p, 4)) Cancel_Route(file, 3);
        else memset(file, 0, sizeof(*file));
        return 0;
    }
    return Reply_Error(peer, frame->request_id, PROTO_BAD_TRANSFER, "invalid transfer sequence or offset", 0);
}

static int Handle_Frame(const ProtocolFrame *frame, void *context)
{
    ChatPeer *peer = context;
    ChatServer *server = peer->server;
    const unsigned char *p = frame->payload;
    uint32_t request = frame->request_id;
    unsigned i;
    if (peer->closing) return 0;
    if (request == 0 || request <= peer->last_request)
        return Reply_Error(peer, request, PROTO_BAD_REQUEST_ID, "request ID must increase", 1);
    peer->last_request = request;
    if (frame->type != PROTO_HELLO && frame->type != PROTO_LIST_REQUEST
        && frame->type != PROTO_CHAT_SEND && frame->type != PROTO_PING && frame->type != PROTO_GOODBYE
        && frame->type != PROTO_STATUS_REQUEST && frame->type != PROTO_FILE_OFFER
        && frame->type != PROTO_FILE_DECISION && frame->type != PROTO_FILE_DATA && frame->type != PROTO_FILE_ACK
        && frame->type != PROTO_FILE_END && frame->type != PROTO_FILE_DONE && frame->type != PROTO_FILE_CANCEL)
        return Reply_Error(peer, request, PROTO_BAD_PAYLOAD, "wrong frame direction", 1);
    if (frame->type == PROTO_STATUS_REQUEST) {
        if (Send_Status(peer, request)) return -1;
        if (!peer->active) Begin_Close(peer);
        return 0;
    }
    if (!peer->active && frame->type != PROTO_HELLO)
        return Reply_Error(peer, request, PROTO_BAD_STATE, "HELLO required", 1);

    switch (frame->type) {
    case PROTO_FILE_OFFER: case PROTO_FILE_DECISION: case PROTO_FILE_DATA: case PROTO_FILE_ACK:
    case PROTO_FILE_END: case PROTO_FILE_DONE: case PROTO_FILE_CANCEL: return File_Frame(peer, frame);
    case PROTO_HELLO: {
        size_t nick = frame->length - 2;
        unsigned char id[4];
        if (peer->active) return Reply_Error(peer, request, PROTO_BAD_STATE, "already registered", 0);
        for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i) {
            ChatPeer *other = &server->peers[i];
            if (Peer_Active(other) && strlen(other->nickname) == nick
                && memcmp(other->nickname, p + 2, nick) == 0)
                return Reply_Error(peer, request, PROTO_NICK_IN_USE, "nickname is in use", 0);
        }
        if (Active_Count(server) >= server->max_users || server->next_id == 0)
            return Reply_Error(peer, request, PROTO_SERVER_FULL, "participant IDs exhausted", 1);
        Protocol_Write32(id, server->next_id);
        if (Queue_Frame(peer, PROTO_WELCOME, request, id, 4) != 0) return -1;
        /* Регистрация становится видимой только после постановки WELCOME.
           Увеличение UINT32_MAX даёт 0, который запрещает дальнейшую выдачу ID. */
        peer->id = server->next_id++;
        memcpy(peer->nickname, p + 2, nick); peer->nickname[nick] = 0;
        peer->active = 1; server->roster_dirty = 1;
        return Send_Status(peer, 0);
    }
    case PROTO_LIST_REQUEST:
        return Send_Roster(peer, request) == 0 ? 0 : -1;
    case PROTO_PING:
        return Queue_Frame(peer, PROTO_PONG, request, NULL, 0) == 0 ? 0 : -1;
    case PROTO_GOODBYE:
        Begin_Close(peer); return 0;
    case PROTO_CHAT_SEND: {
        ChatPeer *recipient = NULL;
        uint32_t recipient_id = Protocol_Read32(p);
        unsigned char event[PROTOCOL_PAYLOAD_MAX], acknowledgement[4];
        size_t event_length = frame->length + 4;
        for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i)
            if (Peer_Active(&server->peers[i]) && server->peers[i].id == recipient_id)
                recipient = &server->peers[i];
        if (recipient == NULL || recipient == peer)
            return Reply_Error(peer, request, PROTO_RECIPIENT_UNAVAILABLE, "recipient unavailable", 0);
        if (NETWORK_TX_CAPACITY - recipient->connection->tx_size < (int)(PROTOCOL_HEADER_SIZE + event_length)
            || NETWORK_TX_CAPACITY - peer->connection->tx_size < PROTOCOL_HEADER_SIZE + 4)
            return Reply_Error(peer, request, PROTO_QUEUE_FULL, "message was not queued", 0);
        Protocol_Write32(event, peer->id); Protocol_Write32(event + 4, request);
        memcpy(event + 8, p + 4, frame->length - 4);
        Protocol_Write32(acknowledgement, recipient_id);
        /* Один владелец, нет системного I/O между проверкой двух ёмкостей и
           копированием кадров. Обе постановки целые и заранее обеспечены местом. */
        if (Queue_Frame(recipient, PROTO_CHAT_EVENT, 0, event, event_length) != 0) return -1;
        return Queue_Frame(peer, PROTO_ACCEPTED, request, acknowledgement, 4) == 0 ? 0 : -1;
    }
    default: return -1;
    }
}

static void Observe_Connection(Client *connection, ServerConnectionEvent event, void *context)
{
    ChatServer *server = context;
    unsigned i;
    if (event == SERVER_CONNECTION_OPEN) {
        for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i) {
            ChatPeer *peer = &server->peers[i];
            if (peer->connection != NULL) continue;
            memset(peer, 0, sizeof(*peer));
            peer->server = server; peer->connection = connection;
            peer->connected_at = GetTickCount(); Protocol_Init(&peer->decoder);
            return;
        }
    } else {
        ChatPeer *peer = Find_Connection(server, connection);
        if (peer != NULL) {
            Drop_Files(peer, 5);
            if (peer->active) server->roster_dirty = 1;
            /* Никакой указатель на транспортный слот не переживает CLOSE. */
            peer->connection = NULL; peer->active = 0;
        }
    }
}

static int Handle_Data(ServerLoop *loop, Client *connection, const char *data, int length, void *context)
{
    ChatServer *server = context;
    ChatPeer *peer = Find_Connection(server, connection);
    unsigned long before;
    (void)loop;
    if (peer == NULL) return -1;
    before = peer->decoder.frames_started;
    if (Protocol_Feed(&peer->decoder, data, (size_t)length, Handle_Frame, peer) < 0) return -1;
    if (peer->decoder.used != 0 && before != peer->decoder.frames_started)
        peer->frame_started_at = GetTickCount();
    return 0;
}

ChatServer *ChatServer_Create(u_short port)
{ return ChatServer_CreateConfigured(port, SERVER_CLIENT_LIMIT, PROTOCOL_FILE_DEFAULT); }

ChatServer *ChatServer_CreateConfigured(u_short port, unsigned max_users, uint64_t max_file_bytes)
{
    ChatServer *server;
    int error;
    if (!max_users || max_users > PROTOCOL_USERS_MAX || max_file_bytes > PROTOCOL_FILE_MAX) { WSASetLastError(WSAEINVAL); return NULL; }
    server = calloc(1, sizeof(*server));
    if (server == NULL) { WSASetLastError(WSAENOBUFS); return NULL; }
    server->max_users = max_users; server->max_file_bytes = max_file_bytes;
    server->next_id = server->next_transfer = 1;
    server->loop = ServerLoop_CreateConfigured(port, max_users + 1);
    if (server->loop == NULL || ServerLoop_SetLifecycle(server->loop, Observe_Connection, server) < 0) {
        error = WSAGetLastError(); ChatServer_Destroy(server); WSASetLastError(error); return NULL;
    }
    return server;
}

void ChatServer_Destroy(ChatServer *server)
{
    if (server == NULL) return;
    /* Сначала закрытие транспорта с последними lifecycle-уведомлениями;
       контекст приложения до их окончания остаётся живым. */
    ServerLoop_Destroy(server->loop);
    free(server);
}

u_short ChatServer_Port(const ChatServer *server)
{
    return server == NULL ? 0 : ServerLoop_Port(server->loop);
}

int ChatServer_Poll(ChatServer *server, int timeout_ms)
{
    unsigned i;
    DWORD now;
    if (server == NULL) { WSASetLastError(WSAEINVAL); return -1; }
    if (ServerLoop_Poll(server->loop, timeout_ms, Handle_Data, server) < 0) return -1;
    now = GetTickCount();
    for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i) {
        ChatPeer *peer = &server->peers[i];
        if (peer->connection == NULL) continue;
        if (Client_GetState(peer->connection) != CLIENT_OPEN) {
            Drop_Files(peer, 5);
            if (peer->active) server->roster_dirty = 1;
            peer->active = 0; peer->closing = 1;
        }
        if ((!peer->active && !peer->closing && now - peer->connected_at >= PROTOCOL_TIMEOUT_MS)
            || (peer->decoder.used != 0 && now - peer->frame_started_at >= PROTOCOL_TIMEOUT_MS))
            ServerLoop_Disconnect(server->loop, peer->connection);
    }
    for (i = 0; i < SERVER_CLIENT_CAPACITY / 2; ++i) {
        FileRoute *file = &server->files[i];
        if (file->state && now - file->progress_at >= (file->state == 1 ? 60000u : 30000u)) Cancel_Route(file, 4);
    }
    if (server->roster_dirty) {
        server->roster_dirty = 0;
        for (i = 0; i < SERVER_CLIENT_CAPACITY; ++i) {
            ChatPeer *peer = &server->peers[i];
            if (Peer_Active(peer) && Send_Roster(peer, 0) != 0) Begin_Close(peer);
        }
        /* Если переполненная очередь привела к исключению участника,
           следующий проход опубликует уточнённый снимок всем оставшимся. */
    }
    return 0;
}
