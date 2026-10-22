#include "server_book.h"
#include "network.h"
#include <winerror.h>
#include <stdio.h>
#include <string.h>

int ServerAddress_Parse(const char *text, ServerAddress *address)
{
    unsigned parts[4], port = DEFAULT_PORT, i;
    const char *p = text;
    ServerAddress item;
    if (!strcmp(text, "default") || !*text) { strcpy(item.ip, "127.0.0.1"); item.port = DEFAULT_PORT; *address = item; return 1; }
    for (i = 0; i < 4; ++i) {
        unsigned n = 0, digits = 0;
        while (*p >= '0' && *p <= '9') { if (++digits > 3) return 0; n = n * 10 + (unsigned)(*p++ - '0'); }
        if (!digits || n > 255 || (i < 3 && *p++ != '.')) return 0;
        parts[i] = n;
    }
    if (*p) {
        unsigned digits = 0;
        if (*p != ':' && *p != ' ') return 0;
        ++p; while (*p == ' ') ++p;
        port = 0;
        while (*p >= '0' && *p <= '9') { if (++digits > 5) return 0; port = port * 10 + (unsigned)(*p++ - '0'); }
        if (!digits || !port || port > 65535) return 0;
    }
    if (*p) return 0;
    snprintf(item.ip, sizeof(item.ip), "%u.%u.%u.%u", parts[0], parts[1], parts[2], parts[3]); item.port = (uint16_t)port;
    *address = item; return 1;
}

int ServerBook_Init(ServerBook *book, const wchar_t *directory)
{
    memset(book, 0, sizeof(*book)); ServerAddress_Parse("", &book->entries[0]); book->count = 1;
    if (directory) {
        if (!*directory || wcslen(directory) > 990) return 0;
        wcscpy(book->directory, directory);
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", book->directory, 950);
        if (!n || n >= 950) return 0;
        wcscat(book->directory, L"\\WinSock");
    }
    wcscpy(book->path, book->directory); wcscat(book->path, L"\\servers.wss"); return 1;
}

void ServerBook_Add(ServerBook *book, const ServerAddress *address)
{
    unsigned i;
    if (!strcmp(book->entries[0].ip, address->ip) && book->entries[0].port == address->port) return;
    for (i = 1; i < book->count; ++i) if (!strcmp(book->entries[i].ip, address->ip) && book->entries[i].port == address->port) break;
    if (i == book->count && book->count < SERVER_BOOK_MAX) ++book->count;
    if (i >= SERVER_BOOK_MAX) i = SERVER_BOOK_MAX - 1;
    memmove(book->entries + 2, book->entries + 1, (i - 1) * sizeof(ServerAddress)); book->entries[1] = *address;
}

int ServerBook_Load(ServerBook *book)
{
    HANDLE file = CreateFileW(book->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    char bytes[1025], *p;
    DWORD size, high, read;
    ServerBook candidate = *book;
    unsigned count = 0;
    candidate.count = 1;
    if (file == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
    size = GetFileSize(file, &high);
    if (high || size < 5 || size > 1024 || !ReadFile(file, bytes, size, &read, NULL) || read != size) { CloseHandle(file); return 0; }
    CloseHandle(file); bytes[size] = 0;
    if (memcmp(bytes, "WSS1\n", 5) || memchr(bytes, 0, size)) return 0;
    p = bytes + 5;
    while (*p) {
        char *end = strchr(p, '\n');
        ServerAddress address;
        unsigned i;
        if (!end || end == p || ++count > SERVER_BOOK_MAX) return 0;
        *end = 0;
        if (!ServerAddress_Parse(p, &address)) return 0;
        for (i = 1; i < candidate.count; ++i) if (!strcmp(candidate.entries[i].ip, address.ip) && candidate.entries[i].port == address.port) return 0;
        /* Сохраняем порядок файла; стандартный адрес уже закреплён в нулевой позиции. */
        if (strcmp(address.ip, candidate.entries[0].ip) || address.port != candidate.entries[0].port) {
            if (candidate.count == SERVER_BOOK_MAX) return 0;
            candidate.entries[candidate.count++] = address;
        }
        p = end + 1;
    }
    *book = candidate; return 1;
}

int ServerBook_Save(const ServerBook *book)
{
    char bytes[1024], suffix[64];
    wchar_t path[1280], dir[1024], wide_suffix[64];
    size_t at = 5, i;
    DWORD written;
    HANDLE file;
    int ok;
    if (!*book->path) return 0;
    memcpy(bytes, "WSS1\n", 5);
    for (i = 0; i < book->count; ++i) at += (size_t)snprintf(bytes + at, sizeof(bytes) - at, "%s:%u\n", book->entries[i].ip, book->entries[i].port);
    wcscpy(dir, book->directory);
    for (i = 1; dir[i]; ++i) if ((dir[i] == '\\' || dir[i] == '/') && dir[i - 1] != ':') {
        wchar_t c = dir[i]; dir[i] = 0;
        if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
        dir[i] = c;
    }
    if (!CreateDirectoryW(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
    snprintf(suffix, sizeof(suffix), ".%lu.tmp", (unsigned long)GetCurrentProcessId());
    MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide_suffix, 64); wcscpy(path, book->path); wcscat(path, wide_suffix);
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(file, bytes, (DWORD)at, &written, NULL) && written == at;
    if (!FlushFileBuffers(file)) ok = 0;
    if (!CloseHandle(file)) ok = 0;
    if (ok) ok = MoveFileExW(path, book->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(path);
    return ok;
}
