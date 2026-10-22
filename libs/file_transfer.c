#include "file_transfer.h"
#include "network.h"
#include <winerror.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* CNG вызывается через системную DLL: старый MinGW не поставляет bcrypt.h/lib.
   SHA-256 предоставляет Windows; собственного криптографического алгоритма здесь нет. */
typedef LONG (WINAPI *HashOpen)(void **, LPCWSTR, LPCWSTR, ULONG);
typedef LONG (WINAPI *HashCreate)(void *, void **, PUCHAR, ULONG, PUCHAR, ULONG, ULONG);
typedef LONG (WINAPI *HashData)(void *, PUCHAR, ULONG, ULONG);
typedef LONG (WINAPI *HashFinish)(void *, PUCHAR, ULONG, ULONG);
typedef LONG (WINAPI *HashDestroy)(void *);
typedef LONG (WINAPI *HashClose)(void *, ULONG);
typedef struct {
    HMODULE dll;
    void *algorithm, *hash;
    HashOpen open;
    HashCreate create;
    HashData data;
    HashFinish finish;
    HashDestroy destroy;
    HashClose close;
} Digest;

enum { PH_NONE, PH_ID, PH_WAIT, PH_SEND, PH_ACK, PH_DONE, PH_OFFER, PH_RECEIVE, PH_FINISHED };
struct FileTransfer {
    FileInfo info;
    FilePost post;
    void *context;
    HANDLE file;
    Digest digest;
    wchar_t target[1024], temporary[1200];
    int temporary_owned, phase, next_phase;
    DWORD progress_at;
    uint32_t last_request, cancel_wait;
    uint8_t pending_type, control_type;
    unsigned char pending[PROTOCOL_PAYLOAD_MAX], control[5];
    size_t pending_length;
    uint64_t block_end;
};

static void Digest_Close(Digest *d)
{
    if (d->hash && d->destroy) d->destroy(d->hash);
    if (d->algorithm && d->close) d->close(d->algorithm, 0);
    if (d->dll) FreeLibrary(d->dll);
    memset(d, 0, sizeof(*d));
}
static int Digest_Open(Digest *d)
{
    wchar_t path[1024];
    UINT n = GetSystemDirectoryW(path, 990);
    FARPROC address;
    if (!n || n >= 990) return 0;
    wcscat(path, L"\\bcrypt.dll"); d->dll = LoadLibraryW(path);
    if (!d->dll) return 0;
#define LOAD(member, name) do { address = GetProcAddress(d->dll, name); memcpy(&d->member, &address, sizeof(d->member)); if (!d->member) goto failed; } while (0)
    LOAD(open, "BCryptOpenAlgorithmProvider"); LOAD(create, "BCryptCreateHash"); LOAD(data, "BCryptHashData");
    LOAD(finish, "BCryptFinishHash"); LOAD(destroy, "BCryptDestroyHash"); LOAD(close, "BCryptCloseAlgorithmProvider");
#undef LOAD
    /* Windows 7+ управляет памятью объекта при NULL/0, освобождает её DestroyHash. */
    if (d->open(&d->algorithm, L"SHA256", NULL, 0) < 0 || d->create(d->algorithm, &d->hash, NULL, 0, NULL, 0, 0) < 0) goto failed;
    return 1;
failed:
    Digest_Close(d); return 0;
}
static void Close_Files(FileTransfer *t)
{
    if (t->file != INVALID_HANDLE_VALUE) { CloseHandle(t->file); t->file = INVALID_HANDLE_VALUE; }
    Digest_Close(&t->digest);
    if (t->temporary_owned) { DeleteFileW(t->temporary); t->temporary_owned = 0; }
}
static int Path(const char *text, wchar_t *wide, int capacity)
{
    size_t n = strlen(text);
    if (n >= 2 && text[0] == '"' && text[n - 1] == '"') { ++text; n -= 2; }
    if (!n || n > 3000) return 0;
    { int result = MultiByteToWideChar(CP_UTF8, 0x8, text, (int)n, wide, capacity - 1);
      if (!result) return 0;
      wide[result] = 0; return 1; }
}
static void Packet(FileTransfer *t, uint8_t type, size_t length, int next_phase)
{ t->pending_type = type; t->pending_length = length; t->next_phase = next_phase; }
static void Control(FileTransfer *t, uint8_t type, uint32_t id, unsigned char reason)
{ Protocol_Write32(t->control, id); t->control[4] = reason; t->control_type = type; }

FileTransfer *FileTransfer_Create(FilePost post, void *context)
{
    FileTransfer *t;
    if (!post) return NULL;
    t = calloc(1, sizeof(*t));
    if (t) { t->post = post; t->context = context; t->file = INVALID_HANDLE_VALUE; }
    return t;
}
int FileTransfer_Busy(const FileTransfer *t)
{ return (t->info.status >= FILE_OFFERING && t->info.status <= FILE_VERIFYING) || t->cancel_wait || t->control_type; }
const FileInfo *FileTransfer_Info(const FileTransfer *t) { return &t->info; }

int FileTransfer_Send(FileTransfer *t, uint32_t recipient, const char *path)
{
    wchar_t wide[1024], *name, *p;
    LARGE_INTEGER size;
    unsigned char frame[PROTOCOL_FRAME_MAX];
    if (FileTransfer_Busy(t) || !recipient || !Path(path, wide, 1024)) return 0;
    Close_Files(t); memset(&t->info, 0, sizeof(t->info)); t->last_request = 0; t->phase = PH_NONE;
    name = wide;
    for (p = wide; *p; ++p) if (*p == L'\\' || *p == L'/') name = p + 1;
    if (!WideCharToMultiByte(CP_UTF8, 0x80, name, -1, t->info.name, sizeof(t->info.name), NULL, NULL)) goto failed;
    t->file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (t->file == INVALID_HANDLE_VALUE) goto failed;
    if (!GetFileSizeEx(t->file, &size)) goto failed;
    if (size.QuadPart < 0 || (uint64_t)size.QuadPart > PROTOCOL_FILE_MAX) { SetLastError(ERROR_INVALID_DATA); goto failed; }
    if (!Digest_Open(&t->digest)) goto failed;
    Protocol_Write32(t->pending, recipient); Protocol_Write64(t->pending + 4, (uint64_t)size.QuadPart);
    Protocol_Write16(t->pending + 12, (uint16_t)strlen(t->info.name)); memcpy(t->pending + 14, t->info.name, strlen(t->info.name));
    if (!Protocol_Encode(frame, sizeof(frame), PROTO_FILE_OFFER, 1, t->pending, 14 + strlen(t->info.name))) goto failed;
    t->info.peer = recipient; t->info.total = (uint64_t)size.QuadPart; t->info.outgoing = 1; t->info.status = FILE_OFFERING;
    t->progress_at = GetTickCount(); Packet(t, PROTO_FILE_OFFER, 14 + strlen(t->info.name), PH_ID); return 1;
failed:
    t->info.error = GetLastError(); t->info.status = FILE_FAILED; Close_Files(t); return 0;
}

int FileTransfer_Decide(FileTransfer *t, uint32_t id, const char *path)
{
    wchar_t suffix[80];
    char narrow[80];
    if (t->info.status != FILE_OFFERED || t->info.id != id) return 0;
    if (!path) { Control(t, PROTO_FILE_DECISION, id, 0); t->info.status = FILE_REFUSED; t->phase = PH_NONE; return 1; }
    if (!Path(path, t->target, 950)) return 0;
    if (GetFileAttributesW(t->target) != INVALID_FILE_ATTRIBUTES) { t->info.error = ERROR_FILE_EXISTS; return 0; }
    snprintf(narrow, sizeof(narrow), ".wsck-%lu-%lu.part", (unsigned long)id, (unsigned long)GetCurrentProcessId());
    MultiByteToWideChar(CP_UTF8, 0, narrow, -1, suffix, 80); wcscpy(t->temporary, t->target); wcscat(t->temporary, suffix);
    t->file = CreateFileW(t->temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (t->file == INVALID_HANDLE_VALUE) { t->info.error = GetLastError(); return 0; }
    t->temporary_owned = 1;
    if (!Digest_Open(&t->digest)) { Close_Files(t); return 0; }
    Protocol_Write32(t->pending, id); t->pending[4] = 1;
    t->info.status = FILE_RECEIVING; t->progress_at = GetTickCount(); Packet(t, PROTO_FILE_DECISION, 5, PH_RECEIVE); return 1;
}

void FileTransfer_Cancel(FileTransfer *t, unsigned char reason)
{
    if (t->info.id && t->info.status >= FILE_OFFERING && t->info.status <= FILE_VERIFYING)
        Control(t, PROTO_FILE_CANCEL, t->info.id, reason);
    else if (t->info.status == FILE_OFFERING && t->last_request) t->cancel_wait = t->last_request;
    t->info.error = reason; t->info.status = t->info.saved ? FILE_COMPLETED : reason == 1 ? FILE_CANCELLED : FILE_FAILED;
    t->pending_type = 0; t->phase = PH_NONE; Close_Files(t);
}

void FileTransfer_Frame(FileTransfer *t, const ProtocolFrame *frame)
{
    const unsigned char *p = frame->payload;
    uint32_t id;
    if (frame->type == PROTO_ERROR) {
        if (frame->request_id == t->cancel_wait) t->cancel_wait = 0;
        if (frame->request_id == t->last_request && FileTransfer_Busy(t)) {
            FileTransfer_Cancel(t, 3); t->cancel_wait = 0; /* ERROR завершил запрос, нового CREATED уже не ждём. */
        }
        return;
    }
    if (frame->type == PROTO_FILE_OFFER_EVENT) {
        if (FileTransfer_Busy(t)) { Control(t, PROTO_FILE_DECISION, Protocol_Read32(p), 0); return; }
        Close_Files(t); memset(&t->info, 0, sizeof(t->info)); t->last_request = 0;
        t->info.id = Protocol_Read32(p); t->info.peer = Protocol_Read32(p + 4); t->info.total = Protocol_Read64(p + 8);
        memcpy(t->info.name, p + 18, frame->length - 18); t->info.status = FILE_OFFERED; t->phase = PH_OFFER;
        t->progress_at = GetTickCount(); return;
    }
    if (frame->type == PROTO_FILE_CREATED) {
        if (frame->request_id == t->cancel_wait) { Control(t, PROTO_FILE_CANCEL, Protocol_Read32(p), 1); t->cancel_wait = 0; return; }
        if (t->phase != PH_ID || frame->request_id != t->last_request || Protocol_Read32(p + 4) != t->info.peer) return;
        t->info.id = Protocol_Read32(p); t->info.status = FILE_WAITING; t->phase = PH_WAIT; t->progress_at = GetTickCount(); return;
    }
    if (frame->length < 4 || (id = Protocol_Read32(p)) != t->info.id || !id) return;
    if (frame->type == PROTO_FILE_CANCEL) {
        Close_Files(t); t->pending_type = 0; t->phase = PH_NONE; t->info.error = p[4];
        t->info.status = t->info.saved ? FILE_COMPLETED : p[4] == 1 ? FILE_CANCELLED : FILE_FAILED; return;
    }
    if (frame->type == PROTO_FILE_DECISION_EVENT && t->phase == PH_WAIT) {
        if (!p[4]) { Close_Files(t); t->info.status = FILE_REFUSED; t->phase = PH_NONE; }
        else { t->info.status = FILE_SENDING; t->phase = PH_SEND; }
        t->progress_at = GetTickCount(); return;
    }
    if (frame->type == PROTO_FILE_ACK && t->phase == PH_ACK && Protocol_Read64(p + 4) == t->block_end) {
        t->info.bytes = t->block_end; t->phase = PH_SEND; t->progress_at = GetTickCount(); return;
    }
    if (frame->type == PROTO_FILE_DATA && t->phase == PH_RECEIVE && !t->pending_type
        && Protocol_Read64(p + 4) == t->info.bytes && frame->length - 12 <= t->info.total - t->info.bytes) {
        size_t at = 12;
        /* Синхронная файловая запись тоже может быть частичной: ACK уходит только
           после всего блока и успешного обновления хеша. Размер памяти постоянен. */
        while (at < frame->length) {
            DWORD written;
            if (!WriteFile(t->file, p + at, (DWORD)(frame->length - at), &written, NULL) || !written) { FileTransfer_Cancel(t, 3); return; }
            at += written;
        }
        if (t->digest.data(t->digest.hash, (PUCHAR)p + 12, (ULONG)frame->length - 12, 0) < 0) { FileTransfer_Cancel(t, 3); return; }
        t->info.bytes += frame->length - 12;
        Protocol_Write32(t->pending, id); Protocol_Write64(t->pending + 4, t->info.bytes);
        Packet(t, PROTO_FILE_ACK, 12, PH_RECEIVE); t->progress_at = GetTickCount(); return;
    }
    if (frame->type == PROTO_FILE_END && t->phase == PH_RECEIVE && !t->pending_type && t->info.bytes == t->info.total) {
        unsigned char hash[32];
        if (t->digest.finish(t->digest.hash, hash, 32, 0) < 0 || memcmp(hash, p + 4, 32)) { FileTransfer_Cancel(t, 6); return; }
        if (!FlushFileBuffers(t->file)) { FileTransfer_Cancel(t, 3); return; }
        if (!CloseHandle(t->file)) { t->file = INVALID_HANDLE_VALUE; FileTransfer_Cancel(t, 3); return; }
        t->file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(t->temporary, t->target, MOVEFILE_WRITE_THROUGH)) { FileTransfer_Cancel(t, 3); return; }
        t->temporary_owned = 0; t->info.saved = 1; Digest_Close(&t->digest); t->info.status = FILE_VERIFYING;
        Protocol_Write32(t->pending, id); Packet(t, PROTO_FILE_DONE, 4, PH_FINISHED); t->progress_at = GetTickCount(); return;
    }
    if (frame->type == PROTO_FILE_DONE && t->phase == PH_DONE) {
        Close_Files(t); t->info.status = FILE_COMPLETED; t->phase = PH_FINISHED; return;
    }
    if (t->info.status >= FILE_OFFERING && t->info.status <= FILE_VERIFYING) FileTransfer_Cancel(t, 3);
}

void FileTransfer_Tick(FileTransfer *t)
{
    DWORD now = GetTickCount();
    uint32_t request;
    if (t->control_type && t->post(t->context, t->control_type, t->control, 5, &request)) t->control_type = 0;
    if (t->cancel_wait && now - t->progress_at >= 60000) t->cancel_wait = 0;
    if (t->info.status >= FILE_OFFERING && t->info.status <= FILE_VERIFYING
        && now - t->progress_at >= (t->info.status <= FILE_OFFERED ? 60000u : 30000u)) { FileTransfer_Cancel(t, 4); return; }
    if (t->pending_type) {
        if (t->post(t->context, t->pending_type, t->pending, t->pending_length, &request)) {
            t->last_request = request; t->phase = t->next_phase; t->pending_type = 0; t->progress_at = now;
            if (t->phase == PH_FINISHED) t->info.status = FILE_COMPLETED;
        }
        return;
    }
    if (t->phase == PH_SEND) {
        uint64_t remaining = t->info.total - t->info.bytes;
        DWORD read;
        Protocol_Write32(t->pending, t->info.id);
        if (!remaining) {
            if (t->digest.finish(t->digest.hash, t->pending + 4, 32, 0) < 0) { FileTransfer_Cancel(t, 3); return; }
            t->info.status = FILE_VERIFYING; Packet(t, PROTO_FILE_END, 36, PH_DONE);
        } else {
            if (remaining > PROTOCOL_FILE_BLOCK) remaining = PROTOCOL_FILE_BLOCK;
            if (!ReadFile(t->file, t->pending + 12, (DWORD)remaining, &read, NULL) || read != remaining
                || t->digest.data(t->digest.hash, t->pending + 12, read, 0) < 0) { FileTransfer_Cancel(t, 3); return; }
            t->block_end = t->info.bytes + read; Protocol_Write64(t->pending + 4, t->info.bytes);
            Packet(t, PROTO_FILE_DATA, read + 12, PH_ACK);
        }
    }
}

void FileTransfer_Destroy(FileTransfer *t)
{ if (t) { Close_Files(t); free(t); } }
