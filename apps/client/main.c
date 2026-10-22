#include "network.h"
#include "chat.h"
#include "protocol.h"
#include <shellapi.h>
#include <stdio.h>
#include <string.h>

static void Usage(void)
{
    fprintf(stderr, "Usage: client [IPv4-address [port: 1..65535 [nickname]]] [--lang en|ru] [--no-history] [--history-dir path]\n");
}

int main(void)
{
    int argc, positional = 0, valid = 1, result;
    size_t i;
    LPWSTR *args = CommandLineToArgvW(GetCommandLineW(), &argc);
    ChatOptions options = {UI_EN, 1, NULL, 0};
    char address[80] = "127.0.0.1", port_text[24], nickname[PROTOCOL_NICK_MAX + 1] = {0};
    const char *nick = NULL;
    u_short port = DEFAULT_PORT;
    if (!args) return 1;
    /* Весь пользовательский ввод приходит из Unicode-командной строки, без
       зависимости от ANSI-кодировки CRT и активной локали Windows. */
    for (i = 1; i < (size_t)argc && valid; ++i) {
        if (!wcscmp(args[i], L"--no-history")) options.save_history = 0;
        else if (!wcscmp(args[i], L"--lang")) {
            if (++i == (size_t)argc) valid = 0;
            else if (!wcscmp(args[i], L"en")) options.language = UI_EN;
            else if (!wcscmp(args[i], L"ru")) options.language = UI_RU;
            else valid = 0;
        } else if (!wcscmp(args[i], L"--history-dir")) {
            if (++i == (size_t)argc || !*args[i]) valid = 0;
            else options.history_directory = args[i];
        } else if (!wcscmp(args[i], L"--help")) {
            Usage(); puts(Ui_Help(options.language)); LocalFree(args); return 0;
        } else if (wcsncmp(args[i], L"--", 2) == 0 || positional >= 3) valid = 0;
        else {
            char *buffer = positional == 0 ? address : positional == 1 ? port_text : nickname;
            int capacity = positional == 0 ? sizeof(address) : positional == 1 ? sizeof(port_text) : sizeof(nickname);
            valid = WideCharToMultiByte(CP_UTF8, 0x80, args[i], -1, buffer, capacity, NULL, NULL) != 0;
            if (valid && positional == 1) valid = Parse_Port(port_text, &port) == 0;
            if (positional == 2) {
                nick = nickname;
                valid = valid && *nick && Protocol_ValidText((const unsigned char *)nick, strlen(nick), 1);
            }
            ++positional;
        }
    }
    if (!valid) { Usage(); LocalFree(args); return 1; }
    options.choose_server = positional == 0;
    result = Chat_Run(address, port, nick, &options);
    LocalFree(args); return result == 0 ? 0 : 1;
}
