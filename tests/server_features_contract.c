/** @file server_features_contract.c @brief Книга адресов, качество и отменяемая проба реального WSCK-сервера. */
#include "server_probe.h"
#include "chat_server.h"
#include "ui_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct { volatile LONG port, stop, failed; } Running;
static DWORD WINAPI Serve(LPVOID context)
{
    Running *r = context;
    ChatServer *server;
    if (Network_Initialize()) { InterlockedExchange(&r->failed, 1); return 0; }
    server = ChatServer_Create(0);
    if (!server) { InterlockedExchange(&r->failed, 1); Network_Cleanup(); return 0; }
    InterlockedExchange(&r->port, ChatServer_Port(server));
    while (!InterlockedCompareExchange(&r->stop, 0, 0)) if (ChatServer_Poll(server, 20)) { InterlockedExchange(&r->failed, 1); break; }
    ChatServer_Destroy(server); Network_Cleanup(); return 0;
}
int main(void)
{
    ServerBook book, loaded;
    ServerAddress address;
    ServerProbe *probe;
    ServerProbeResult result;
    Running running = {0};
    HANDLE thread, file;
    wchar_t directory[1024], suffix[80];
    char text[80];
    DWORD wrote;
    unsigned i;
    CHECK(ServerAddress_Parse("", &address) && !strcmp(address.ip, "127.0.0.1") && address.port == 5050);
    CHECK(ServerAddress_Parse("192.168.001.002:65535", &address) && !strcmp(address.ip, "192.168.1.2") && address.port == 65535);
    CHECK(ServerAddress_Parse("10.1.2.3 1234", &address) && address.port == 1234);
    { const char *bad[] = {"300.1.1.1", "1.2.3", "1.2.3.4:0", "1.2.3.4:65536", "1.2.3.4:12x", "host:5050", "1.2.3.4:999999999999"};
      for (i = 0; i < sizeof(bad)/sizeof(*bad); ++i) CHECK(!ServerAddress_Parse(bad[i], &address)); }
    CHECK(Ui_Quality(0, 1) == 3 && Ui_Quality(100, 1) == 3 && Ui_Quality(101, 1) == 2);
    CHECK(Ui_Quality(250, 1) == 2 && Ui_Quality(251, 1) == 1 && Ui_Quality(1, 0) == 1);
    CHECK(GetCurrentDirectoryW(950, directory)); snprintf(text, sizeof(text), "\\server-book-%lu", (unsigned long)GetCurrentProcessId());
    CHECK(MultiByteToWideChar(CP_UTF8, 0, text, -1, suffix, 80)); wcscat(directory, suffix);
    CHECK(ServerBook_Init(&book, directory)); CHECK(ServerBook_Load(&book));
    for (i = 1; i < 25; ++i) { snprintf(text, sizeof(text), "10.0.0.%u:5050", i); CHECK(ServerAddress_Parse(text, &address)); ServerBook_Add(&book, &address); }
    CHECK(book.count == 16 && !strcmp(book.entries[0].ip, "127.0.0.1") && !strcmp(book.entries[1].ip, "10.0.0.24"));
    ServerBook_Add(&book, &address); CHECK(book.count == 16);
    CHECK(ServerBook_Save(&book)); CHECK(ServerBook_Init(&loaded, directory)); CHECK(ServerBook_Load(&loaded));
    CHECK(loaded.count == 16 && !strcmp(loaded.entries[1].ip, "10.0.0.24"));
    file = CreateFileW(book.path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(file, "BAD!", 4, &wrote, NULL)); CloseHandle(file);
    CHECK(!ServerBook_Load(&loaded) && loaded.count == 16);
    CHECK(DeleteFileW(book.path)); CHECK(RemoveDirectoryW(directory));
    thread = CreateThread(NULL, 0, Serve, &running, 0, NULL); CHECK(thread);
    for (i = 0; i < 400 && !InterlockedCompareExchange(&running.port, 0, 0) && !InterlockedCompareExchange(&running.failed, 0, 0); ++i) Sleep(5);
    CHECK(running.port && !running.failed); CHECK(ServerAddress_Parse("", &address)); address.port = (uint16_t)running.port;
    probe = ServerProbe_Start(&address); CHECK(probe);
    for (i = 0; i < 400 && !ServerProbe_Read(probe, &result); ++i) Sleep(5);
    CHECK(i < 400 && result.online && result.users == 0 && !result.error && result.latency < 1500);
    CHECK(result.capacity == 10 && result.file_limit == PROTOCOL_FILE_DEFAULT);
    ServerProbe_Destroy(probe);
    probe = ServerProbe_Start(&address); CHECK(probe);
    { DWORD began = GetTickCount(); ServerProbe_Destroy(probe); CHECK(GetTickCount() - began < 500); }
    InterlockedExchange(&running.stop, 1); CHECK(WaitForSingleObject(thread, 2000) == WAIT_OBJECT_0); CloseHandle(thread); CHECK(!running.failed);
    puts("PASS: IPv4/port parsing, quality thresholds, bounded persistent server book, real anonymous status and cancellation"); return 0;
}
