#include "command_catalog.h"
#include <string.h>

static const CommandHint commands[] = {
    {"/help", "", "Commands and shortcuts", "Команды и сочетания клавиш"},
    {"/list", "", "Refresh participants", "Обновить участников"},
    {"/to", "ID", "Choose a conversation", "Выбрать собеседника"},
    {"/msg", "ID text", "Send a private message", "Отправить личное сообщение"},
    {"/sendfile", "[ID] [path]", "Choose and send a file", "Выбрать и отправить файл"},
    {"/accept", "[TRANSFER] [path]", "Accept the incoming file", "Принять входящий файл"},
    {"/reject", "[TRANSFER]", "Decline the incoming file", "Отклонить входящий файл"},
    {"/cancel", "", "Cancel file transfer", "Отменить передачу"},
    {"/ping", "", "Measure server response", "Измерить задержку"},
    {"/status", "", "Server status and limits", "Статус и лимиты сервера"},
    {"/servers", "", "Saved servers", "Сохранённые серверы"},
    {"/connect", "IPv4:port", "Connect to another server", "Подключиться к другому серверу"},
    {"/disconnect", "", "Leave server, open menu", "Выйти с сервера в меню"},
    {"/lang", "en|ru", "Switch language", "Сменить язык"},
    {"/history", "on|off|save|clear", "Local history", "Локальная история"},
    {"/quit", "", "Exit the application", "Завершить приложение"}
};

const CommandHint *Command_Match(const char *input, unsigned index)
{
    size_t i, n = strlen(input);
    if (*input != '/' || strpbrk(input, " \t\r\n")) return NULL;
    for (i = 0; i < sizeof(commands) / sizeof(*commands); ++i)
        if (!strncmp(input, commands[i].name, n) && index-- == 0) return &commands[i];
    return NULL;
}
unsigned Command_Count(const char *input)
{
    unsigned n = 0;
    while (Command_Match(input, n)) ++n;
    return n;
}
