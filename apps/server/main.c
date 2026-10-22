/** @file main_server.c
 * @brief Сервер WSCK v2 с настраиваемыми пределами участников и файлов.
 */
#include "chat_server.h"
#include "branding.h"
#include <stdio.h>
#include <string.h>

/* Разбор без переполнения и без знака; ограничения проверяются до умножения. */
static int Limit(const char *text, uint64_t ceiling, uint64_t *result)
{
    uint64_t n = 0;
    if (!*text) return 0;
    while (*text) {
        unsigned d = (unsigned)(*text++ - '0');
        if (d > 9 || n > (ceiling - d) / 10) return 0;
        n = n * 10 + d;
    }
    *result = n; return 1;
}

/* Обработчик консольного события сообщает только об остановке. Сокеты
   остаются собственностью основного потока, который выполняет очистку. */
static volatile LONG stop_requested;

static BOOL WINAPI Request_Stop(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) return FALSE;
    InterlockedExchange(&stop_requested, 1);
    return TRUE;
}

int main(int argc, char **argv)
{
    ChatServer *loop;
    u_short port = DEFAULT_PORT;
    int result;
    unsigned max_users = SERVER_CLIENT_LIMIT;
    uint64_t max_file = PROTOCOL_FILE_DEFAULT, value;
    int i, positional = 0, file_option = 0;
    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--max-clients") && i + 1 < argc) {
            if (!Limit(argv[++i], PROTOCOL_USERS_MAX, &value) || !value) goto usage;
            max_users = (unsigned)value;
        } else if ((!strcmp(argv[i], "--max-file-gib") || !strcmp(argv[i], "--max-file-bytes")) && i + 1 < argc) {
            int gib = !strcmp(argv[i], "--max-file-gib");
            if (file_option++ || !Limit(argv[++i], gib ? 1024 : PROTOCOL_FILE_MAX, &value)) goto usage;
            max_file = gib ? value * UINT64_C(1073741824) : value;
        } else if (!positional++ && Parse_Port(argv[i], &port) == 0) {
            /* Совместимый позиционный порт. */
        } else goto usage;
    }
    result = Network_Initialize();
    if (result != 0) {
        fprintf(stderr, "WSAStartup failed: %s (%d)\n", Network_ErrorText(result), result);
        return 1;
    }
    loop = ChatServer_CreateConfigured(port, max_users, max_file);
    if (loop == NULL) {
        int error = WSAGetLastError();
        fprintf(stderr, "Server initialization failed: %s (%d)\n", Network_ErrorText(error), error);
        Network_Cleanup();
        return 1;
    }
    if (!SetConsoleCtrlHandler(Request_Stop, TRUE)) {
        fprintf(stderr, "Console handler failed: %lu\n", (unsigned long)GetLastError());
        ChatServer_Destroy(loop);
        Network_Cleanup();
        return 1;
    }
    fputs(TERMTALK_BANNER, stdout);
    printf("Listening on port %u; limit %u clients; file limit %llu bytes. Ctrl+C to stop.\n",
           (unsigned)ChatServer_Port(loop), max_users, (unsigned long long)max_file);
    fflush(stdout);
    result = 0;
    while (InterlockedCompareExchange(&stop_requested, 0, 0) == 0) {
        if (ChatServer_Poll(loop, 100) < 0) {
            int error = WSAGetLastError();
            fprintf(stderr, "Server loop failed: %s (%d)\n", Network_ErrorText(error), error);
            result = 1;
            break;
        }
    }
    ChatServer_Destroy(loop);
    SetConsoleCtrlHandler(Request_Stop, FALSE);
    Network_Cleanup();
    return result;
usage:
    fprintf(stderr, "Usage: server [port: 1..65535] [--max-clients 1..32] [--max-file-gib 0..1024 | --max-file-bytes N]\nDefaults: 10 users, 32 GiB per file; 0 disables files.\n");
    return 1;
}
