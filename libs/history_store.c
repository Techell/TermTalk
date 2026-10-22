#include "history_store.h"
#include "network.h"
#include <winerror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t Hash(const unsigned char *p, size_t n)
{
    uint32_t h = 2166136261u;
    while (n--) { h ^= *p++; h *= 16777619u; }
    return h;
}

int History_Init(HistoryStore *store, const UiModel *model, const wchar_t *directory)
{
    char scope[192], suffix[64];
    wchar_t wide_suffix[64];
    uint32_t a, b;
    memset(store, 0, sizeof(*store));
    if (directory) {
        if (!*directory || wcslen(directory) >= 1000) return 0;
        wcscpy(store->directory, directory);
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", store->directory, 950);
        if (!n || n >= 950) return 0;
        wcscat(store->directory, L"\\WinSock\\history");
    }
    snprintf(scope, sizeof(scope), "%s|%s", model->endpoint, model->nickname);
    a = Hash((const unsigned char *)scope, strlen(scope));
    b = Hash((const unsigned char *)model->nickname, strlen(model->nickname));
    /* Никнейм никогда не становится частью пути. Полная область профиля также
       записана в заголовке: коллизия хеша не позволяет загрузить чужой журнал. */
    snprintf(suffix, sizeof(suffix), "\\%08lx-%08lx.wsh", (unsigned long)a, (unsigned long)b);
    if (!MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide_suffix, 64)) return 0;
    wcscpy(store->path, store->directory); wcscat(store->path, wide_suffix); return 1;
}

int History_Load(const HistoryStore *store, UiModel *model)
{
    HANDLE file;
    unsigned char *data;
    UiMessage *messages;
    DWORD size, high, read;
    size_t at, end;
    unsigned count, scope, nick, i;
    int valid = 0;
    DWORD attributes = GetFileAttributesW(store->path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError() == ERROR_FILE_NOT_FOUND
        || GetLastError() == ERROR_PATH_NOT_FOUND;
    file = CreateFileW(store->path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    size = GetFileSize(file, &high);
    if (high || size < 16 || size > 1100000) { CloseHandle(file); return 0; }
    data = malloc((size_t)size); messages = calloc(UI_HISTORY_MAX, sizeof(*messages));
    if (!data || !messages) { free(data); free(messages); CloseHandle(file); return 0; }
    if (!ReadFile(file, data, size, &read, NULL) || read != size) goto finish;
    if (memcmp(data, "WSH1", 4) || Protocol_Read32(data + size - 4) != Hash(data, (size_t)size - 4)) goto finish;
    scope = Protocol_Read16(data + 4); nick = Protocol_Read16(data + 6); count = Protocol_Read16(data + 8);
    at = 12; end = (size_t)size - 4;
    if (data[10] || data[11] || count > UI_HISTORY_MAX || scope != strlen(model->endpoint)
        || nick != strlen(model->nickname) || at + scope + nick > end
        || memcmp(data + at, model->endpoint, scope) || memcmp(data + at + scope, model->nickname, nick)) goto finish;
    at += scope + nick;
    for (i = 0; i < count; ++i) {
        UiMessage *m = &messages[i];
        unsigned pn, tn;
        uint64_t stamp;
        if (at + 16 > end) goto finish;
        m->outgoing = data[at]; pn = Protocol_Read16(data + at + 2); tn = Protocol_Read32(data + at + 4);
        stamp = ((uint64_t)Protocol_Read32(data + at + 8) << 32) | Protocol_Read32(data + at + 12);
        if (m->outgoing > 1 || data[at + 1] || stamp > 4102444800ULL || !pn || pn > PROTOCOL_NICK_MAX
            || !tn || tn > PROTOCOL_TEXT_MAX || at + 16 + pn + tn > end) goto finish;
        at += 16;
        if (!Protocol_ValidText(data + at, pn, 1) || !Protocol_ValidText(data + at + pn, tn, 0)) goto finish;
        memcpy(m->peer, data + at, pn); memcpy(m->text, data + at + pn, tn); at += pn + tn;
        m->timestamp = (time_t)stamp;
        if ((uint64_t)m->timestamp != stamp) goto finish;
        m->delivery = UI_RESTORED;
    }
    if (at != end) goto finish;
    memcpy(model->messages, messages, count * sizeof(*messages)); model->message_count = count;
    valid = 1;
finish:
    CloseHandle(file); free(messages); free(data); return valid;
}

static int Create_Directories(const wchar_t *path)
{
    wchar_t copy[1024];
    size_t i;
    wcscpy(copy, path);
    for (i = 1; copy[i]; ++i) if ((copy[i] == L'\\' || copy[i] == L'/') && copy[i - 1] != L':') {
        wchar_t separator = copy[i]; copy[i] = 0;
        if (!CreateDirectoryW(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return 0;
        copy[i] = separator;
    }
    return CreateDirectoryW(copy, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
}

int History_Save(const HistoryStore *store, const UiModel *model)
{
    unsigned char *data = malloc(1100000);
    wchar_t temporary[1280], wide_suffix[64];
    char suffix[64];
    HANDLE file;
    DWORD written;
    size_t at = 12, scope = strlen(model->endpoint), nick = strlen(model->nickname);
    unsigned i;
    int ok;
    if (!data) return 0;
    memcpy(data, "WSH1", 4); Protocol_Write16(data + 4, (uint16_t)scope);
    Protocol_Write16(data + 6, (uint16_t)nick); Protocol_Write16(data + 8, (uint16_t)model->message_count);
    data[10] = data[11] = 0;
    memcpy(data + at, model->endpoint, scope); at += scope;
    memcpy(data + at, model->nickname, nick); at += nick;
    for (i = 0; i < model->message_count; ++i) {
        const UiMessage *m = &model->messages[i];
        size_t pn = strlen(m->peer), tn = strlen(m->text);
        uint64_t stamp = (uint64_t)m->timestamp;
        data[at] = (unsigned char)m->outgoing; data[at + 1] = 0;
        Protocol_Write16(data + at + 2, (uint16_t)pn); Protocol_Write32(data + at + 4, (uint32_t)tn);
        Protocol_Write32(data + at + 8, (uint32_t)(stamp >> 32)); Protocol_Write32(data + at + 12, (uint32_t)stamp);
        at += 16; memcpy(data + at, m->peer, pn); at += pn; memcpy(data + at, m->text, tn); at += tn;
    }
    Protocol_Write32(data + at, Hash(data, at)); at += 4;
    if (!Create_Directories(store->directory)) { free(data); return 0; }
    snprintf(suffix, sizeof(suffix), ".%lu.tmp", (unsigned long)GetCurrentProcessId());
    MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide_suffix, 64);
    wcscpy(temporary, store->path); wcscat(temporary, wide_suffix);
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) { free(data); return 0; }
    ok = WriteFile(file, data, (DWORD)at, &written, NULL) && written == at;
    if (!FlushFileBuffers(file)) ok = 0;
    if (!CloseHandle(file)) ok = 0;
    free(data);
    /* Замена выполняется только после успешной записи; старый файл не открываем
       на усечение. Временный файл всегда находится на том же томе. */
    if (ok) ok = MoveFileExW(temporary, store->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temporary);
    return ok;
}

int History_Clear(const HistoryStore *store)
{
    if (DeleteFileW(store->path)) return 1;
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}
