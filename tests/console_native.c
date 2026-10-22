/** @file console_native.c @brief Проверка настоящего Windows screen buffer в собственной скрытой консоли. */
#include "console_ui.h"
#include "file_browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define CHECK(c) do { if (!(c)) { ExitProcess((UINT)__LINE__); } } while (0)

static void Type(HANDLE input, const wchar_t *text)
{
    DWORD count;
    INPUT_RECORD event = {0};
    event.EventType = KEY_EVENT; event.Event.KeyEvent.bKeyDown = TRUE; event.Event.KeyEvent.wRepeatCount = 1;
    while (*text) {
        event.Event.KeyEvent.uChar.UnicodeChar = *text;
        event.Event.KeyEvent.wVirtualKeyCode = *text == L'\r' ? VK_RETURN : 0;
        CHECK(WriteConsoleInputW(input, &event, 1, &count)); ++text;
    }
}

static void Press(WORD code)
{
    INPUT_RECORD event = {0}; DWORD count;
    event.EventType = KEY_EVENT; event.Event.KeyEvent.bKeyDown = TRUE;
    event.Event.KeyEvent.wRepeatCount = 1; event.Event.KeyEvent.wVirtualKeyCode = code;
    if (code == VK_RETURN) event.Event.KeyEvent.uChar.UnicodeChar = L'\r';
    if (code == VK_TAB) event.Event.KeyEvent.uChar.UnicodeChar = L'\t';
    CHECK(WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &event, 1, &count));
}
static void Control(WORD code)
{
    INPUT_RECORD event = {0}; DWORD count;
    event.EventType = KEY_EVENT; event.Event.KeyEvent.bKeyDown = TRUE;
    event.Event.KeyEvent.wRepeatCount = 1; event.Event.KeyEvent.wVirtualKeyCode = code;
    event.Event.KeyEvent.dwControlKeyState = LEFT_CTRL_PRESSED;
    event.Event.KeyEvent.uChar.UnicodeChar = (WCHAR)(code - 'A' + 1);
    CHECK(WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &event, 1, &count));
}
static void Screen(const wchar_t *text, int snapshot)
{
    DWORD began = GetTickCount();
    wchar_t content[320 * 160 + 1] = {0};
    while (GetTickCount() - began < 5000) {
        HANDLE screen = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        CONSOLE_SCREEN_BUFFER_INFO info;
        CHAR_INFO *cells; int width, height, i; SMALL_RECT region;
        CHECK(screen != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(screen, &info));
        width = info.srWindow.Right - info.srWindow.Left + 1; height = info.srWindow.Bottom - info.srWindow.Top + 1;
        CHECK(width <= 320 && height <= 160); region = info.srWindow;
        cells = calloc((size_t)width * height, sizeof(*cells)); CHECK(cells);
        CHECK(ReadConsoleOutputW(screen, cells, (COORD){(SHORT)width, (SHORT)height}, (COORD){0, 0}, &region));
        for (i = 0; i < width * height; ++i) content[i] = cells[i].Char.UnicodeChar ? cells[i].Char.UnicodeChar : L' ';
        content[width * height] = 0;
        if (wcsstr(content, text)) {
            if (snapshot) {
                HANDLE file; DWORD written;
                file = CreateFileW(L"native-file-picker.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); CHECK(file != INVALID_HANDLE_VALUE);
                CHECK(WriteFile(file, &width, sizeof(width), &written, NULL)); CHECK(WriteFile(file, &height, sizeof(height), &written, NULL));
                CHECK(WriteFile(file, cells, (DWORD)(width * height * sizeof(*cells)), &written, NULL)); CloseHandle(file);
            }
            free(cells); CloseHandle(screen); return;
        }
        if (GetTickCount() - began >= 4900) {
            FILE *log = fopen("native-failure.txt", "wb"); char *utf8 = malloc(320 * 160 * 4 + 1);
            if (log && utf8) { int n = WideCharToMultiByte(CP_UTF8, 0, content, -1, utf8, 320 * 160 * 4 + 1, NULL, NULL); if (n) fwrite(utf8, 1, (size_t)n - 1, log); }
            if (log) fclose(log);
            free(utf8);
        }
        free(cells); CloseHandle(screen); Sleep(5);
    }
    {
        char expected[1024]; FILE *trace = fopen("native-screen-failure.txt", "wb");
        int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, expected, sizeof(expected), NULL, NULL);
        if (trace) {
            int capacity = WideCharToMultiByte(CP_UTF8, 0, content, -1, NULL, 0, NULL, NULL);
            char *actual = malloc((size_t)capacity);
            if (n) fwrite(expected, 1, (size_t)n - 1, trace);
            fputs("\nActual screen:\n", trace);
            if (actual && WideCharToMultiByte(CP_UTF8, 0, content, -1, actual, capacity, NULL, NULL)) fwrite(actual, 1, (size_t)capacity - 1, trace);
            free(actual); fclose(trace);
        }
    }
    CHECK(0);
}
static void Send_Frame(SOCKET peer, uint8_t type, uint32_t request, const unsigned char *payload, size_t length)
{
    unsigned char bytes[PROTOCOL_FRAME_MAX]; size_t at = 0, n = Protocol_Encode(bytes, sizeof(bytes), type, request, payload, length);
    CHECK(n);
    while (at < n) { int sent = send(peer, (const char *)bytes + at, (int)(n - at), 0); CHECK(sent > 0); at += (size_t)sent; }
}
static void Exact(SOCKET peer, unsigned char *output, size_t length)
{
    size_t at = 0;
    while (at < length) { int n = recv(peer, (char *)output + at, (int)(length - at), 0); CHECK(n > 0); at += (size_t)n; }
}
static uint32_t Wanted(SOCKET peer, uint8_t type, unsigned char *payload, size_t expected)
{
    unsigned char header[16]; size_t length; uint32_t request;
    DWORD began = GetTickCount();
    for (;;) {
        CHECK(GetTickCount() - began < 5000);
        Exact(peer, header, sizeof(header)); CHECK(!memcmp(header, "WSCK", 4) && header[4] == PROTOCOL_VERSION);
        length = Protocol_Read32(header + 8); request = Protocol_Read32(header + 12); CHECK(length <= PROTOCOL_PAYLOAD_MAX);
        Exact(peer, payload, length);
        if (header[5] == PROTO_PING && type != PROTO_PING) { Send_Frame(peer, PROTO_PONG, request, NULL, 0); continue; }
        CHECK(header[5] == type && length == expected); return request;
    }
}
static const unsigned char abc_hash[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
};
static void Incoming(SOCKET peer, uint32_t id, const char *name)
{
    unsigned char payload[256]; size_t n = strlen(name);
    Protocol_Write32(payload, id); Protocol_Write32(payload + 4, 2); Protocol_Write64(payload + 8, 3);
    Protocol_Write16(payload + 16, (uint16_t)n); memcpy(payload + 18, name, n);
    Send_Frame(peer, PROTO_FILE_OFFER_EVENT, 0, payload, n + 18);
    Screen(L"File offer #", 0);
}
static void Receive_ABC(SOCKET peer, uint32_t id, const char *target)
{
    unsigned char payload[PROTOCOL_PAYLOAD_MAX]; wchar_t wide[1024]; HANDLE file; DWORD read;
    Wanted(peer, PROTO_FILE_DECISION, payload, 5); CHECK(Protocol_Read32(payload) == id && payload[4] == 1);
    Protocol_Write32(payload, id); Protocol_Write64(payload + 4, 0); memcpy(payload + 12, "abc", 3);
    Send_Frame(peer, PROTO_FILE_DATA, 0, payload, 15);
    Wanted(peer, PROTO_FILE_ACK, payload, 12); CHECK(Protocol_Read64(payload + 4) == 3);
    Protocol_Write32(payload, id); memcpy(payload + 4, abc_hash, 32); Send_Frame(peer, PROTO_FILE_END, 0, payload, 36);
    Wanted(peer, PROTO_FILE_DONE, payload, 4); CHECK(Protocol_Read32(payload) == id);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, target, -1, wide, 1024));
    file = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(ReadFile(file, payload, 4, &read, NULL) && read == 3 && !memcmp(payload, "abc", 3)); CloseHandle(file); CHECK(DeleteFileW(wide));
}
static void File_Client(const char *client_path)
{
    SOCKET listener, peer; struct sockaddr_in address = {0}; int size = sizeof(address);
    STARTUPINFOW startup = {0}; PROCESS_INFORMATION process;
    char root[BROWSER_PATH_MAX], folder[BROWSER_PATH_MAX], source[BROWSER_PATH_MAX], target[BROWSER_PATH_MAX], name[80], suffix[160];
    wchar_t command[1400], executable[1024], wide[1200];
    unsigned char payload[PROTOCOL_PAYLOAD_MAX]; uint32_t request; DWORD timeout = 5000, written, code; HANDLE file;
    CHECK(Browser_ClientDirectory(0, root, sizeof(root)));
    snprintf(folder, sizeof(folder), "%sпанель-%lu", root, (unsigned long)GetCurrentProcessId());
    CHECK(MultiByteToWideChar(CP_UTF8, 0, folder, -1, wide, 1200)); CHECK(CreateDirectoryW(wide, NULL));
    snprintf(source, sizeof(source), "%s\\исходный.txt", folder); CHECK(MultiByteToWideChar(CP_UTF8, 0, source, -1, wide, 1200));
    file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(file, "abc", 3, &written, NULL)); CloseHandle(file);
    CHECK(Network_Initialize() == 0); listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(listener != INVALID_SOCKET);
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(listener, (const struct sockaddr *)&address, sizeof(address))); CHECK(!getsockname(listener, (struct sockaddr *)&address, &size)); CHECK(!listen(listener, 1));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, client_path, -1, executable, 1024));
    snprintf(suffix, sizeof(suffix), "\" 127.0.0.1 %u PanelUser --no-history", ntohs(address.sin_port));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide, 1200));
    wcscpy(command, L"\""); wcscat(command, executable); wcscat(command, wide);
    startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process));
    { fd_set ready; struct timeval wait = {5, 0}; FD_ZERO(&ready); FD_SET(listener, &ready); CHECK(select(0, &ready, NULL, NULL, &wait) == 1); }
    peer = accept(listener, NULL, NULL); CHECK(peer != INVALID_SOCKET); CHECK(!setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)));
    Wanted(peer, PROTO_HELLO, payload, 11); Protocol_Write32(payload, 1); Send_Frame(peer, PROTO_WELCOME, 1, payload, 4);
    {
        const char *names[2] = {"PanelUser", "Peer"}; unsigned i; size_t at = 2;
        Protocol_Write16(payload, 2);
        for (i = 0; i < 2; ++i) {
            size_t n = strlen(names[i]); Protocol_Write32(payload + at, i + 1); payload[at + 4] = 1; Protocol_Write16(payload + at + 5, (uint16_t)n); at += 7;
            memcpy(payload + at, names[i], n); at += n; payload[at] = 127; payload[at + 1] = payload[at + 2] = 0; payload[at + 3] = 1;
            Protocol_Write16(payload + at + 4, 1234); at += 6;
        }
        Send_Frame(peer, PROTO_LIST_RESPONSE, 0, payload, at);
    }
    Type(GetStdHandle(STD_INPUT_HANDLE), L"/to 2\r"); Screen(L"Selected recipient 2.", 0);
    Type(GetStdHandle(STD_INPUT_HANDLE), L"/sen"); Screen(L"/sendfile", 0); Press(VK_TAB); Press(VK_RETURN);
    Screen(L"SELECT FILE TO SEND", 0);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, folder, -1, wide, 1200)); Type(GetStdHandle(STD_INPUT_HANDLE), wide); Press(VK_RETURN);
    Screen(L"исходный.txt", 0);
    Protocol_Write32(payload, 2); Protocol_Write32(payload + 4, 99); memcpy(payload + 8, "message while browsing", 22);
    Send_Frame(peer, PROTO_CHAT_EVENT, 0, payload, 30);
    Protocol_Write16(payload, 2); Protocol_Write16(payload + 2, 10); Protocol_Write64(payload + 4, PROTOCOL_FILE_DEFAULT);
    Send_Frame(peer, PROTO_STATUS_RESPONSE, 0, payload, 12); Screen(L"2/10", 0);
    Screen(L"исходный.txt", 1); Press(VK_DOWN); Press(VK_RETURN); Screen(L"File transfer #0", 0);
    Screen(L"message while browsing", 0);
    request = Wanted(peer, PROTO_FILE_OFFER, payload, 14 + strlen("исходный.txt")); CHECK(Protocol_Read32(payload) == 2 && Protocol_Read64(payload + 4) == 3);
    Protocol_Write32(payload, 10); Protocol_Write32(payload + 4, 2); Send_Frame(peer, PROTO_FILE_CREATED, request, payload, 8);
    Protocol_Write32(payload, 10); payload[4] = 1; Send_Frame(peer, PROTO_FILE_DECISION_EVENT, 0, payload, 5);
    Wanted(peer, PROTO_FILE_DATA, payload, 15); CHECK(Protocol_Read64(payload + 4) == 0 && !memcmp(payload + 12, "abc", 3));
    Protocol_Write32(payload, 10); Protocol_Write64(payload + 4, 3); Send_Frame(peer, PROTO_FILE_ACK, 0, payload, 12);
    Wanted(peer, PROTO_FILE_END, payload, 36); CHECK(!memcmp(payload + 4, abc_hash, 32)); Protocol_Write32(payload, 10); Send_Frame(peer, PROTO_FILE_DONE, 0, payload, 4);
    Screen(L"File completed #10", 0);
    Type(GetStdHandle(STD_INPUT_HANDLE), L"/status\r"); request = Wanted(peer, PROTO_STATUS_REQUEST, payload, 0);
    Protocol_Write16(payload, 2); Protocol_Write16(payload + 2, 10); Protocol_Write64(payload + 4, PROTOCOL_FILE_DEFAULT);
    Send_Frame(peer, PROTO_STATUS_RESPONSE, request, payload, 12); Screen(L"Server online: 2/10", 0);
    Press(VK_UP); Press(VK_RETURN); request = Wanted(peer, PROTO_STATUS_REQUEST, payload, 0);
    Protocol_Write16(payload, 2); Protocol_Write16(payload + 2, 10); Protocol_Write64(payload + 4, PROTOCOL_FILE_DEFAULT);
    Send_Frame(peer, PROTO_STATUS_RESPONSE, request, payload, 12);
    Type(GetStdHandle(STD_INPUT_HANDLE), L"черновик🙂"); Press(VK_UP); Press(VK_DOWN); Screen(L"черновик", 0); Control('U');
    snprintf(name, sizeof(name), "native-%lu.bin", (unsigned long)GetCurrentProcessId());
    Incoming(peer, 20, name); Type(GetStdHandle(STD_INPUT_HANDLE), L"/accept\r"); Screen(L"downloads (next to client)", 0); Press(VK_RETURN);
    snprintf(target, sizeof(target), "%sdownloads\\%s", root, name); Receive_ABC(peer, 20, target);
    Incoming(peer, 21, name); Type(GetStdHandle(STD_INPUT_HANDLE), L"/accept\r"); Screen(L"downloads (next to client)", 0); Press(VK_DOWN); Press(VK_RETURN);
    Screen(L"SAVE INCOMING FILE", 0); CHECK(MultiByteToWideChar(CP_UTF8, 0, folder, -1, wide, 1200)); Type(GetStdHandle(STD_INPUT_HANDLE), wide); Press(VK_RETURN);
    Screen(L"исходный.txt", 0); Press(VK_F7); snprintf(target, sizeof(target), "%s\\%s", folder, name); Receive_ABC(peer, 21, target);
    Incoming(peer, 22, name); Type(GetStdHandle(STD_INPUT_HANDLE), L"/accept\r"); Screen(L"downloads (next to client)", 0);
    Protocol_Write32(payload, 22); payload[4] = 4; Send_Frame(peer, PROTO_FILE_CANCEL, 0, payload, 5); Screen(L"The file offer has ended.", 0);
    Incoming(peer, 23, name); Type(GetStdHandle(STD_INPUT_HANDLE), L"/reject\r"); Wanted(peer, PROTO_FILE_DECISION, payload, 5); CHECK(Protocol_Read32(payload) == 23 && !payload[4]);
    Type(GetStdHandle(STD_INPUT_HANDLE), L"/quit\r"); Wanted(peer, PROTO_GOODBYE, payload, 0);
    CHECK(recv(peer, (char *)payload, 1, 0) == 0); shutdown(peer, SD_SEND); closesocket(peer); closesocket(listener);
    CHECK(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0); CHECK(GetExitCodeProcess(process.hProcess, &code) && !code);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); Network_Cleanup();
    CHECK(MultiByteToWideChar(CP_UTF8, 0, source, -1, wide, 1200)); CHECK(DeleteFileW(wide));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, folder, -1, wide, 1200)); CHECK(RemoveDirectoryW(wide));
}

static void Login_Client(const char *client_path, int menu)
{
    SOCKET listener, peer;
    struct sockaddr_in address = {0};
    int size = sizeof(address), bytes, total, round;
    unsigned char data[PROTOCOL_FRAME_MAX], payload[100];
    wchar_t executable[1024], command[1200], wide_suffix[80];
    char suffix[80];
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process;
    DWORD code, timeout = 5000;
    CHECK(Network_Initialize() == 0);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(listener != INVALID_SOCKET);
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(bind(listener, (const struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(getsockname(listener, (struct sockaddr *)&address, &size) == 0); CHECK(listen(listener, 1) == 0);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, client_path, -1, executable, 1024));
    if (menu) snprintf(suffix, sizeof(suffix), "\" --no-history --lang ru");
    else snprintf(suffix, sizeof(suffix), "\" 127.0.0.1 %u --no-history --lang ru", ntohs(address.sin_port));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide_suffix, 80));
    wcscpy(command, L"\""); wcscat(command, executable); wcscat(command, wide_suffix);
    startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process));
    if (menu) {
        snprintf(suffix, sizeof(suffix), "127.0.0.1:%u", ntohs(address.sin_port));
        CHECK(MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide_suffix, 80));
        Type(GetStdHandle(STD_INPUT_HANDLE), wide_suffix);
        Press(VK_F7); Screen(L"Добро пожаловать", 0);
        Type(GetStdHandle(STD_INPUT_HANDLE), L"Алиса\r"); Screen(L"Никнейм сохранён", 0); Press(VK_RETURN);
    }
    if (!menu) Type(GetStdHandle(STD_INPUT_HANDLE), L"Алиса\r");
    for (round = 0; round < (menu ? 2 : 1); ++round) {
    { fd_set readable; struct timeval wait = {5, 0}; FD_ZERO(&readable); FD_SET(listener, &readable);
      CHECK(select(0, &readable, NULL, NULL, &wait) == 1); }
    peer = accept(listener, NULL, NULL); CHECK(peer != INVALID_SOCKET);
    CHECK(setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)) == 0);
    total = 0;
    while (total < 28) { bytes = recv(peer, (char *)data + total, 28 - total, 0); CHECK(bytes > 0); total += bytes; }
    CHECK(!memcmp(data, "WSCK", 4) && data[5] == PROTO_HELLO && Protocol_Read32(data + 12) == 1);
    CHECK(Protocol_Read16(data + 16) == 10 && !memcmp(data + 18, "Алиса", 10));
    Protocol_Write32(payload, 1);
    size = (int)Protocol_Encode(data, sizeof(data), PROTO_WELCOME, 1, payload, 4);
    CHECK(send(peer, (const char *)data, size, 0) == size);
    Protocol_Write16(payload, 1); Protocol_Write32(payload + 2, 1); payload[6] = 1; Protocol_Write16(payload + 7, 10);
    memcpy(payload + 9, "Алиса", 10); payload[19] = 127; payload[20] = payload[21] = 0; payload[22] = 1; Protocol_Write16(payload + 23, 1234);
    size = (int)Protocol_Encode(data, sizeof(data), PROTO_LIST_RESPONSE, 0, payload, 25);
    CHECK(send(peer, (const char *)data, size, 0) == size);
    if (menu && round == 0) {
        INPUT_RECORD key = {0}; DWORD count;
        key.EventType = KEY_EVENT; key.Event.KeyEvent.bKeyDown = TRUE;
        key.Event.KeyEvent.wRepeatCount = 1; key.Event.KeyEvent.wVirtualKeyCode = VK_F4;
        CHECK(WriteConsoleInputW(GetStdHandle(STD_INPUT_HANDLE), &key, 1, &count));
    } else Type(GetStdHandle(STD_INPUT_HANDLE), L"/quit\r");
    total = 0;
    while (total < 16) { bytes = recv(peer, (char *)data + total, 16 - total, 0); CHECK(bytes > 0); total += bytes; }
    CHECK(data[5] == PROTO_GOODBYE && Protocol_Read32(data + 12) == 2); CHECK(recv(peer, (char *)data, 1, 0) == 0);
    CHECK(shutdown(peer, SD_SEND) == 0); closesocket(peer);
    if (menu && round == 0) Type(GetStdHandle(STD_INPUT_HANDLE), L"\r");
    }
    closesocket(listener);
    CHECK(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); Network_Cleanup();
}

/* Отказ регистрации не должен запирать пользователя в цикле с прежним именем.
   Проверяем настоящий контроллер, отмену формы и сохранение адреса между вводами. */
static void Rename_Client(const char *client_path)
{
    SOCKET listener, peer;
    struct sockaddr_in address = {0}; int size = sizeof(address), attempt;
    STARTUPINFOW startup = {0}; PROCESS_INFORMATION process;
    wchar_t executable[1024], command[1400], wide[100]; char suffix[100];
    unsigned char payload[PROTOCOL_PAYLOAD_MAX], header[16]; DWORD code, timeout = 5000;
    CHECK(Network_Initialize() == 0);
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); CHECK(listener != INVALID_SOCKET);
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK(!bind(listener, (const struct sockaddr *)&address, sizeof(address)));
    CHECK(!getsockname(listener, (struct sockaddr *)&address, &size)); CHECK(!listen(listener, 4));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, client_path, -1, executable, 1024));
    snprintf(suffix, sizeof(suffix), "\" 127.0.0.1 %u Busy --no-history", ntohs(address.sin_port));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide, 100));
    wcscpy(command, L"\""); wcscat(command, executable); wcscat(command, wide);
    startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    CHECK(CreateProcessW(NULL, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process));
    { fd_set ready; struct timeval wait = {5, 0}; FD_ZERO(&ready); FD_SET(listener, &ready); CHECK(select(0, &ready, NULL, NULL, &wait) == 1); }
    peer = accept(listener, NULL, NULL); CHECK(peer != INVALID_SOCKET);
    CHECK(!setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)));
    Wanted(peer, PROTO_HELLO, payload, 6); CHECK(!memcmp(payload + 2, "Busy", 4));
    Protocol_Write16(payload, PROTO_NICK_IN_USE); Protocol_Write16(payload + 2, 4); memcpy(payload + 4, "busy", 4);
    Send_Frame(peer, PROTO_ERROR, 1, payload, 8); shutdown(peer, SD_SEND); closesocket(peer);
    Screen(L"Nickname is already in use", 0); Screen(L"F7 Nickname", 0); Screen(L"TermTalk", 0);
    snprintf(suffix, sizeof(suffix), "127.0.0.1:%u", ntohs(address.sin_port));
    CHECK(MultiByteToWideChar(CP_UTF8, 0, suffix, -1, wide, 100)); Type(GetStdHandle(STD_INPUT_HANDLE), wide);
    Press(VK_F7); Screen(L"Welcome. Choose your nickname", 0);
    Control('U'); Press(VK_RETURN); Screen(L"Nickname must contain", 0);
    Type(GetStdHandle(STD_INPUT_HANDLE), L"Discard"); Press(VK_ESCAPE); Screen(L"SERVERS", 0); Screen(L"Busy", 0); Screen(wide, 0);
    Press(VK_F7); Control('U'); Type(GetStdHandle(STD_INPUT_HANDLE), L"Зоя\r");
    Screen(L"Nickname saved", 0); Screen(wide, 0); Press(VK_RETURN);
    /* Проба статуса из меню может опередить явный Connect: обслуживаем её,
       не принимая за регистрацию и не завязывая тест на скорость машины. */
    peer = INVALID_SOCKET;
    for (attempt = 0; attempt < 5; ++attempt) {
        fd_set ready; struct timeval wait = {5, 0}; unsigned length;
        FD_ZERO(&ready); FD_SET(listener, &ready); CHECK(select(0, &ready, NULL, NULL, &wait) == 1);
        peer = accept(listener, NULL, NULL); CHECK(peer != INVALID_SOCKET);
        CHECK(!setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout)));
        Exact(peer, header, 16); length = Protocol_Read32(header + 8); CHECK(length <= sizeof(payload)); Exact(peer, payload, length);
        if (header[5] == PROTO_HELLO) { CHECK(length == 8 && Protocol_Read16(payload) == 6 && !memcmp(payload + 2, "Зоя", 6)); break; }
        CHECK(header[5] == PROTO_STATUS_REQUEST && !length);
        Protocol_Write16(payload, 0); Protocol_Write16(payload + 2, 10); Protocol_Write64(payload + 4, PROTOCOL_FILE_DEFAULT);
        Send_Frame(peer, PROTO_STATUS_RESPONSE, Protocol_Read32(header + 12), payload, 12);
        shutdown(peer, SD_SEND); closesocket(peer); peer = INVALID_SOCKET;
    }
    CHECK(peer != INVALID_SOCKET && attempt < 5);
    Protocol_Write32(payload, 1); Send_Frame(peer, PROTO_WELCOME, 1, payload, 4);
    Screen(L"Registered as", 0); Type(GetStdHandle(STD_INPUT_HANDLE), L"/quit\r");
    Wanted(peer, PROTO_GOODBYE, payload, 0); shutdown(peer, SD_SEND); closesocket(peer); closesocket(listener);
    CHECK(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0);
    CHECK(GetExitCodeProcess(process.hProcess, &code) && code == 0);
    CloseHandle(process.hThread); CloseHandle(process.hProcess); Network_Cleanup();
}

static int Child(const char *client_path)
{
    ConsoleUi ui;
    UiModel *m = calloc(1, sizeof(*m));
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE), original = GetStdHandle(STD_OUTPUT_HANDLE), active;
    DWORD old_mode, mode, count;
    CONSOLE_SCREEN_BUFFER_INFO info;
    CHAR_INFO marker = {{0}, 15}, *readback;
    INPUT_RECORD event = {0};
    SMALL_RECT region = {0, 0, 0, 0}, resized_window = {0, 0, 59, 21};
    unsigned i;
    CHECK(m); CHECK(GetConsoleMode(input, &old_mode));
    marker.Char.UnicodeChar = L'Z';
    CHECK(WriteConsoleOutputW(original, &marker, (COORD){1, 1}, (COORD){0, 0}, &region));
    CHECK(Console_Open(&ui) && ui.interactive);
    CHECK(GetConsoleMode(input, &mode) && !(mode & (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT)));
    m->language = UI_RU; m->online = 1; strcpy(m->nickname, "Алиса"); strcpy(m->endpoint, "127.0.0.1:5050");
    CHECK(Console_Draw(&ui, m)); CHECK(GetConsoleScreenBufferInfo(ui.screen, &info));
    readback = calloc((size_t)ui.width * ui.height, sizeof(*readback)); CHECK(readback);
    region = info.srWindow;
    CHECK(ReadConsoleOutputW(ui.screen, readback, (COORD){(SHORT)ui.width, (SHORT)ui.height}, (COORD){0, 0}, &region));
    CHECK(!memcmp(ui.cells, readback, (size_t)ui.width * ui.height * sizeof(*readback)));
    free(readback);
    CHECK(FlushConsoleInputBuffer(input));
    event.EventType = KEY_EVENT; event.Event.KeyEvent.bKeyDown = TRUE; event.Event.KeyEvent.wRepeatCount = 1;
    event.Event.KeyEvent.uChar.UnicodeChar = L'Ж';
    CHECK(WriteConsoleInputW(input, &event, 1, &count));
    for (i = 0; i < 30 && !*m->input; ++i) { Console_Poll(&ui, m); Sleep(1); }
    CHECK(!strcmp(m->input, "Ж")); CHECK(Console_Draw(&ui, m));
    CHECK(SetConsoleWindowInfo(ui.screen, TRUE, &resized_window));
    CHECK(SetConsoleScreenBufferSize(ui.screen, (COORD){60, 22}));
    CHECK(Console_Draw(&ui, m) && ui.width == 60 && ui.height == 22);
    Console_Close(&ui); CHECK(GetConsoleMode(input, &mode) && mode == old_mode);
    active = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    CHECK(active != INVALID_HANDLE_VALUE); region = (SMALL_RECT){0, 0, 0, 0}; marker.Char.UnicodeChar = 0;
    CHECK(ReadConsoleOutputW(active, &marker, (COORD){1, 1}, (COORD){0, 0}, &region));
    CHECK(marker.Char.UnicodeChar == L'Z'); CloseHandle(active); Console_Close(&ui); free(m);
    Login_Client(client_path, 0); CHECK(GetConsoleMode(input, &mode) && mode == old_mode);
    Login_Client(client_path, 1); CHECK(GetConsoleMode(input, &mode) && mode == old_mode);
    File_Client(client_path); CHECK(GetConsoleMode(input, &mode) && mode == old_mode);
    Rename_Client(client_path); CHECK(GetConsoleMode(input, &mode) && mode == old_mode); return 0;
}

int main(int argc, char **argv)
{
    wchar_t executable[1024], client[1024], command[2200];
    STARTUPINFOW startup = {0};
    PROCESS_INFORMATION process;
    HANDLE job;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    DWORD code, wait;
    if (argc == 3 && !strcmp(argv[1], "--child")) return Child(argv[2]);
    CHECK(argc == 2); CHECK(MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, client, 1024));
    CHECK(GetModuleFileNameW(NULL, executable, 1024));
    wcscpy(command, L"\""); wcscat(command, executable); wcscat(command, L"\" --child \""); wcscat(command, client); wcscat(command, L"\"");
    startup.cb = sizeof(startup); startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
    job = CreateJobObjectW(NULL, NULL); CHECK(job);
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    CHECK(SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)));
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NEW_CONSOLE | CREATE_SUSPENDED, NULL, NULL, &startup, &process)) {
        fprintf(stderr, "Cannot create isolated console: %lu\n", (unsigned long)GetLastError()); return 1;
    }
    CHECK(AssignProcessToJobObject(job, process.hProcess)); CHECK(ResumeThread(process.hThread) != (DWORD)-1);
    wait = WaitForSingleObject(process.hProcess, 25000);
    if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 255);
    GetExitCodeProcess(process.hProcess, &code); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    CloseHandle(job); /* Ошибка теста тоже не оставляет дочерний клиент работать. */
    if (wait != WAIT_OBJECT_0 || code) { fprintf(stderr, "Native console check failed at line %lu\n", (unsigned long)code); return 1; }
    puts("PASS: real screen readback, Unicode input, resize, console restoration and actual client nickname login"); return 0;
}
