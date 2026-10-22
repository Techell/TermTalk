#include "ui_model.h"
#include <string.h>

void Ui_ClearCommands(UiModel *m)
{
    memset(m->commands, 0, sizeof(m->commands));
    memset(m->command_draft, 0, sizeof(m->command_draft));
    m->command_count = m->command_position = 0; m->command_cursor = 0;
}
int Ui_RememberCommand(UiModel *m, const char *text)
{
    size_t n = strlen(text);
    char saved[UI_INPUT_MAX];
    m->command_position = 0;
    if (n < 2 || n >= UI_INPUT_MAX || *text != '/' || !Protocol_ValidText((const unsigned char *)text, n, 0)) return 0;
    if (m->command_count && !strcmp(m->commands[m->command_count - 1], text)) return 0;
    /* Вход может ссылаться на запись самой истории; сдвиг не должен его изменить. */
    memcpy(saved, text, n + 1);
    if (m->command_count == UI_COMMAND_HISTORY_MAX) {
        memmove(m->commands, m->commands + 1, (UI_COMMAND_HISTORY_MAX - 1) * sizeof(*m->commands)); --m->command_count;
    }
    memcpy(m->commands[m->command_count++], saved, n + 1); return 1;
}
int Ui_RecallCommand(UiModel *m, int direction)
{
    if (direction == -1) {
        if (m->command_position >= m->command_count) return 0;
        if (!m->command_position) {
            strcpy(m->command_draft, m->input); m->command_cursor = m->cursor;
        }
        ++m->command_position;
    } else if (direction == 1) {
        if (!m->command_position) return 0;
        --m->command_position;
    } else return 0;
    if (m->command_position) {
        strcpy(m->input, m->commands[m->command_count - m->command_position]); m->cursor = strlen(m->input);
    } else { strcpy(m->input, m->command_draft); m->cursor = m->command_cursor; }
    m->completion_hidden = 1; m->completion_focus = 0; return 1;
}

unsigned Ui_Completions(const UiModel *m)
{
    return m->completion_hidden || m->file_dialog || m->server_menu || m->help || m->contacts || m->online <= 0
        ? 0 : Command_Count(m->input);
}
void Ui_FileDialog(UiModel *m, int mode)
{
    if (!m->file_dialog && mode) {
        strcpy(m->file_draft, m->input); m->file_cursor = m->cursor;
        m->input[0] = 0; m->cursor = 0;
    } else if (m->file_dialog && !mode) {
        strcpy(m->input, m->file_draft); m->cursor = m->file_cursor;
    }
    m->file_dialog = mode; m->contacts = m->help = 0; m->completion_hidden = 1;
}

const char *Ui_Text(UiLanguage language, const char *en, const char *ru)
{ return language == UI_RU ? ru : en; }

int Ui_Quality(unsigned latency, int responding)
{ return !responding || latency > 250 ? 1 : latency > 100 ? 2 : 3; }

void Ui_ServerMenu(UiModel *m, int open)
{
    if (m->server_menu == open) return;
    if (open) {
        strcpy(m->chat_draft, m->input); m->draft_cursor = m->cursor;
        strcpy(m->input, m->server_draft); m->cursor = m->server_cursor;
    } else {
        strcpy(m->server_draft, m->input); m->server_cursor = m->cursor;
        strcpy(m->input, m->chat_draft); m->cursor = m->draft_cursor;
    }
    m->server_menu = open; m->contacts = m->help = m->scroll = 0;
}

const char *Ui_Help(UiLanguage language)
{
    return Ui_Text(language,
        "/help       Commands and keyboard shortcuts\n"
        "/list       Refresh online participants\n"
        "/to ID      Select a private conversation\n"
        "/msg ID text Send a private message\n"
        "/ping       Check server response\n"
        "/status     Server status, user count and latency\n"
        "/lang en|ru Switch interface language\n"
        "/servers    Open saved servers (F3)\n"
        "/connect IP:port Switch server\n"
        "/disconnect Leave server, open menu (F4)\n"
        "/sendfile [ID] [path] Browse or offer a file\n"
        "/accept [TRANSFER] [path] Choose a save folder\n"
        "/reject [TRANSFER] Decline the incoming file\n"
        "/cancel     Cancel the current file transfer\n"
        "/history on|off Enable/disable local saving\n"
        "/history save   Save the current journal\n"
        "/history clear  Delete this profile's journal\n"
        "/quit       Disconnect and exit\n\n"
        "F1 Help  F2 Language  F3 Servers  F5 Check server\n"
        "F7 Nickname in the disconnected server menu\n"
        "Tab Complete command / Contacts/Input\n"
        "F6 File picker · Enter Open/Send · F7 Save here\n"
        "Arrows Navigate  Enter Select/Send  Esc Back\n"
        "Up/Down Command history (20); Esc hides suggestions\n"
        "PgUp/PgDn Scroll  Ctrl+R Refresh  Ctrl+U Clear input\n"
        "Shift+Enter New line  Ctrl+Q / Ctrl+C Quit\n"
        "ACCEPTED = server queue; delivery is not confirmed.",
        "/help       Команды и сочетания клавиш\n"
        "/list       Обновить список участников\n"
        "/to ID      Выбрать личный разговор\n"
        "/msg ID текст Отправить личное сообщение\n"
        "/ping       Проверить ответ сервера\n"
        "/status     Статус, участники и задержка сервера\n"
        "/lang en|ru Сменить язык интерфейса\n"
        "/servers    Сохранённые серверы (F3)\n"
        "/connect IP:порт Сменить сервер\n"
        "/disconnect Отключиться, открыть меню (F4)\n"
        "/sendfile [ID] [путь] Выбрать или отправить файл\n"
        "/accept [TRANSFER] [путь] Выбрать папку сохранения\n"
        "/reject [TRANSFER] Отклонить входящий файл\n"
        "/cancel     Отменить передачу файла\n"
        "/history on|off Включить/отключить сохранение\n"
        "/history save   Сохранить текущий журнал\n"
        "/history clear  Удалить журнал этого профиля\n"
        "/quit       Отключиться и выйти\n\n"
        "F1 Справка  F2 Язык  F3 Серверы  F5 Проверить сервер\n"
        "F7 Никнейм в меню до подключения к серверу\n"
        "Tab Дополнить команду / Участники/Ввод\n"
        "F6 Файлы · Enter Открыть/Отправить · F7 Сохранить здесь\n"
        "Стрелки Навигация  Enter Выбрать/Отправить  Esc Назад\n"
        "Вверх/Вниз История 20 команд; Esc скрывает подсказки\n"
        "PgUp/PgDn Прокрутка  Ctrl+R Обновить  Ctrl+U Очистить ввод\n"
        "Shift+Enter Новая строка  Ctrl+Q / Ctrl+C Выход\n"
        "ПРИНЯТО = очередь сервера; доставка не подтверждена.");
}

int Ui_Insert(UiModel *model, const char *text)
{
    size_t n = strlen(text), len = strlen(model->input);
    if (!n || len + n >= sizeof(model->input)
        || !Protocol_ValidText((const unsigned char *)text, n, 0)) return 0;
    memmove(model->input + model->cursor + n, model->input + model->cursor, len - model->cursor + 1);
    memcpy(model->input + model->cursor, text, n); model->cursor += n;
    return 1;
}

void Ui_Edit(UiModel *model, int key)
{
    size_t at = model->cursor, next = at, len = strlen(model->input);
    if (key < 0 && at) {
        --next;
        while (next && ((unsigned char)model->input[next] & 0xc0) == 0x80) --next;
        if (key == -2) memmove(model->input + next, model->input + at, len - at + 1);
        model->cursor = next;
    } else if (key > 0 && at < len) {
        ++next;
        while (((unsigned char)model->input[next] & 0xc0) == 0x80) ++next;
        if (key == 2) memmove(model->input + at, model->input + next, len - next + 1);
        else model->cursor = next;
    }
}

UiMessage *Ui_Append(UiModel *model, const UiMessage *message)
{
    if (model->message_count == UI_HISTORY_MAX) {
        memmove(model->messages, model->messages + 1, (UI_HISTORY_MAX - 1) * sizeof(UiMessage));
        --model->message_count;
    }
    model->messages[model->message_count] = *message;
    return &model->messages[model->message_count++];
}

const ProtocolParticipant *Ui_Peer(const UiModel *model, uint32_t id)
{
    unsigned i;
    for (i = 0; i < model->count; ++i) if (model->users[i].id == id) return &model->users[i];
    return NULL;
}
