/** @file ui_contract.c @brief Unicode-редактор, переносы/прокрутка, навигация и атомарная история. */
#include "console_ui.h"
#include "history_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static UiAction Key(ConsoleUi *ui, UiModel *m, WORD code, wchar_t c, DWORD flags)
{
    KEY_EVENT_RECORD event = {TRUE, 1, code, 0, {0}, flags};
    event.uChar.UnicodeChar = c;
    return Console_Key(ui, m, &event);
}

static int Contains(const CHAR_INFO *cells, int w, int h, const wchar_t *text)
{
    int y, x;
    size_t n = wcslen(text), i;
    for (y = 0; y < h; ++y) for (x = 0; x + (int)n <= w; ++x) {
        for (i = 0; i < n && cells[y * w + x + (int)i].Char.UnicodeChar == text[i]; ++i) {}
        if (i == n) return 1;
    }
    return 0;
}

static void Snapshot(const char *path, UiModel *model, CHAR_INFO *cells)
{
    UiMessage msg = {0};
    COORD cursor;
    FILE *file;
    memset(model, 0, sizeof(*model)); model->language = UI_RU; model->saving = model->online = 1;
    strcpy(model->nickname, "Алиса"); strcpy(model->endpoint, "127.0.0.1:5050");
    model->own_id = 1; model->count = 3; model->selected = 2;
    model->users[0].id = 1; strcpy(model->users[0].nickname, "Алиса");
    model->users[1].id = 2; strcpy(model->users[1].nickname, "Михаил");
    model->users[2].id = 3; strcpy(model->users[2].nickname, "Charlie"); model->unread[2] = 2;
    strcpy(msg.peer, "Михаил"); msg.timestamp = 1700000000;
    strcpy(msg.text, "Привет, Алиса! Проверим новый интерфейс?"); Ui_Append(model, &msg);
    msg.outgoing = 1; msg.delivery = UI_ACCEPTED; msg.timestamp += 60;
    strcpy(msg.text, "Привет! Да, русский текст работает. Список участников слева, а историю можно прокручивать клавишами PgUp и PgDn."); Ui_Append(model, &msg);
    msg.outgoing = 0; msg.timestamp += 60;
    strcpy(msg.text, "Отлично. Язык меняется по F2 — прямо во время разговора."); Ui_Append(model, &msg);
    strcpy(model->input, "Попробуем переключиться на English?"); model->cursor = strlen(model->input);
    strcpy(model->notice, "История сохраняется локально · /history off: отключить");
    Console_Layout(model, cells, 110, 32, &cursor);
    file = fopen(path, "wb"); CHECK(file); CHECK(fwrite(cells, sizeof(*cells), 110 * 32, file) == 110 * 32); CHECK(!fclose(file));
}

int main(int argc, char **argv)
{
    UiModel *m = calloc(1, sizeof(*m)), *loaded = calloc(1, sizeof(*loaded));
    UiMessage message = {0};
    ConsoleUi ui = {0};
    HistoryStore store;
    CHAR_INFO *cells = calloc(320 * 160, sizeof(*cells));
    COORD cursor;
    wchar_t directory[1024], suffix[64], blocked_temporary[1280];
    char pid[40];
    HANDLE file;
    DWORD written;
    unsigned i;
    CHECK(m && loaded && cells);
    CHECK(!Ui_RecallCommand(m, -1) && !Ui_RecallCommand(m, 1));
    CHECK(!Ui_RememberCommand(m, "hello") && !Ui_RememberCommand(m, "/") && !Ui_RememberCommand(m, "/bad\x1b"));
    CHECK(Ui_RememberCommand(m, "/ping") && !Ui_RememberCommand(m, "/ping"));
    CHECK(Ui_RememberCommand(m, "/msg 2 Привет🙂"));
    strcpy(m->input, "черновик🙂"); m->cursor = strlen("черно");
    m->online = 1;
    CHECK(Key(&ui, m, VK_UP, 0, 0) == UI_CHANGED && !strcmp(m->input, "/msg 2 Привет🙂"));
    CHECK(!Ui_Completions(m));
    CHECK(Key(&ui, m, VK_UP, 0, 0) == UI_CHANGED && !strcmp(m->input, "/ping"));
    CHECK(Key(&ui, m, VK_UP, 0, 0) == UI_IDLE && !strcmp(m->input, "/ping"));
    CHECK(Key(&ui, m, VK_DOWN, 0, 0) == UI_CHANGED && !strcmp(m->input, "/msg 2 Привет🙂"));
    CHECK(Key(&ui, m, VK_DOWN, 0, 0) == UI_CHANGED && !strcmp(m->input, "черновик🙂") && m->cursor == strlen("черно"));
    CHECK(Key(&ui, m, VK_DOWN, 0, 0) == UI_IDLE && !Ui_RecallCommand(m, 0));
    for (i = 0; i < 25; ++i) { char command[40]; snprintf(command, sizeof(command), "/to %u", i + 1); CHECK(Ui_RememberCommand(m, command)); }
    CHECK(m->command_count == 20 && !strcmp(m->commands[0], "/to 6"));
    CHECK(Ui_RememberCommand(m, m->commands[0]) && !strcmp(m->commands[19], "/to 6"));
    Ui_ClearCommands(m); CHECK(!m->command_count && !m->commands[0][0] && !m->command_draft[0]);
    memset(m, 0, sizeof(*m));
    m->online = 1;
    CHECK(Command_Count("/") == 16 && Command_Count("/s") == 3 && Command_Count("/sendfile ") == 0);
    CHECK(!Command_Count("text") && !Command_Count("/unknown"));
    CHECK(Ui_Insert(m, "/s"));
    CHECK(Ui_Completions(m) == 3);
    Key(&ui, m, VK_DOWN, 0, 0); CHECK(m->completion_focus == 1);
    Console_Layout(m, cells, 110, 32, &cursor);
    CHECK(Contains(cells, 110, 32, L"/sendfile") && Contains(cells, 110, 32, L"/status"));
    CHECK(Key(&ui, m, VK_TAB, 0, 0) == UI_CHANGED && !strcmp(m->input, "/status") && !m->contacts);
    CHECK(!Ui_Completions(m));
    Key(&ui, m, 'U', 0, LEFT_CTRL_PRESSED); Key(&ui, m, 0, L'/', 0);
    CHECK(Ui_Completions(m) == 16); Key(&ui, m, VK_ESCAPE, 0, 0); CHECK(!Ui_Completions(m));
    m->completion_hidden = 0; m->completion_focus = 15;
    for (i = 40; i <= 320; ++i) { Console_Layout(m, cells, (int)i, 18, &cursor); CHECK(cursor.X < (SHORT)i && cursor.Y < 18); }
    m->completion_focus = 0;
    Key(&ui, m, 0, L'a', 0); CHECK(Ui_Completions(m) == 1);
    Key(&ui, m, VK_TAB, 0, 0); CHECK(!strcmp(m->input, "/accept "));
    Key(&ui, m, 'U', 0, LEFT_CTRL_PRESSED); CHECK(Ui_Insert(m, "черновик🙂"));
    Ui_FileDialog(m, 1); CHECK(!*m->input && !Ui_Completions(m));
    strcpy(m->browser.directory, "C:\\Русская папка"); m->browser.count = 2;
    strcpy(m->browser.entries[0].name, "Папка"); m->browser.entries[0].directory = 1;
    strcpy(m->browser.entries[1].name, "файл🙂.bin");
    CHECK(Key(&ui, m, VK_DOWN, 0, 0) == UI_CHANGED && m->browser_focus == 1);
    CHECK(Key(&ui, m, VK_RETURN, 0, 0) == UI_FILE_ENTER);
    CHECK(Key(&ui, m, VK_BACK, 0, 0) == UI_FILE_PARENT);
    CHECK(Key(&ui, m, VK_F6, 0, 0) == UI_FILE_DRIVES);
    Ui_FileDialog(m, 2); CHECK(Key(&ui, m, VK_F7, 0, 0) == UI_FILE_PICK);
    for (i = 1; i <= 320; ++i) { Console_Layout(m, cells, (int)i, 22, &cursor); CHECK(cursor.X < (SHORT)i && cursor.Y < 22); }
    Console_Layout(m, cells, 110, 32, &cursor); CHECK(Contains(cells, 110, 32, L"файл"));
    Ui_FileDialog(m, 3); CHECK(Key(&ui, m, VK_RETURN, 0, 0) == UI_FILE_PICK);
    Key(&ui, m, VK_DOWN, 0, 0); CHECK(Key(&ui, m, VK_RETURN, 0, 0) == UI_FILE_BROWSE);
    CHECK(Key(&ui, m, VK_ESCAPE, 0, 0) == UI_FILE_CLOSE);
    Ui_FileDialog(m, 0); CHECK(!strcmp(m->input, "черновик🙂"));
    memset(m, 0, sizeof(*m));
    strcpy(m->input, "черновик"); m->cursor = strlen(m->input);
    CHECK(Key(&ui, m, VK_F3, 0, 0) == UI_CHANGED && m->server_menu);
    CHECK(!*m->input && Ui_Insert(m, "127.0.0.1:5050"));
    CHECK(Key(&ui, m, VK_F5, 0, 0) == UI_SERVER_CHECK);
    CHECK(Key(&ui, m, VK_RETURN, 0, 0) == UI_SERVER_CONNECT);
    m->online = -3; CHECK(Key(&ui, m, VK_F7, 0, 0) == UI_NICKNAME);
    m->online = 1; CHECK(Key(&ui, m, VK_F7, 0, 0) != UI_NICKNAME);
    Key(&ui, m, VK_ESCAPE, 0, 0); CHECK(!m->server_menu && !strcmp(m->input, "черновик"));
    m->online = 1; m->latency_state = 1; m->capacity = 12; m->latency = 30;
    CHECK(Key(&ui, m, VK_F4, 0, 0) == UI_DISCONNECT);
    Console_Layout(m, cells, 110, 32, &cursor); CHECK(Contains(cells, 110, 32, L"█▀▔"));
    m->latency = 200; Console_Layout(m, cells, 110, 32, &cursor); CHECK(Contains(cells, 110, 32, L"▀▔"));
    m->file.status = FILE_SENDING; m->file.total = PROTOCOL_FILE_DEFAULT; m->file.bytes = UINT64_C(4294967297);
    Console_Layout(m, cells, 110, 32, &cursor); CHECK(Contains(cells, 110, 32, L"4294967297/34359738368"));
    memset(m, 0, sizeof(*m));
    CHECK(Key(&ui, m, 'F', L'Я', 0) == UI_CHANGED);
    CHECK(!strcmp(m->input, "Я")); CHECK(m->cursor == 2);
    CHECK(Ui_Insert(m, "Привет")); Ui_Edit(m, -1); Ui_Edit(m, -2);
    CHECK(!strcmp(m->input, "ЯПривт")); Ui_Edit(m, 2); CHECK(!strcmp(m->input, "ЯПрив"));
    m->cursor = 0; CHECK(Ui_Insert(m, "🙂")); Ui_Edit(m, -2); CHECK(!strcmp(m->input, "ЯПрив"));
    CHECK(!Ui_Insert(m, "\x1b[2J")); CHECK(!Ui_Insert(m, "\xc0\x80"));
    Key(&ui, m, 'U', 0, LEFT_CTRL_PRESSED);
    Key(&ui, m, 0, 0xd83d, 0); Key(&ui, m, 0, 0xde42, 0); CHECK(!strcmp(m->input, "🙂"));
    CHECK(Key(&ui, m, VK_RETURN, L'\r', 0) == UI_SUBMIT);
    Key(&ui, m, VK_RETURN, L'\r', SHIFT_PRESSED); CHECK(!strcmp(m->input, "🙂\n"));
    Key(&ui, m, VK_F2, 0, 0); CHECK(m->language == UI_RU);
    CHECK(strstr(Ui_Help(m->language), "Сменить язык"));
    CHECK(Key(&ui, m, 'Q', 0, LEFT_CTRL_PRESSED) == UI_QUIT);
    strcpy(m->nickname, "Алиса"); strcpy(m->endpoint, "127.0.0.1:5050");
    m->online = 1; m->own_id = 1; m->count = 10;
    for (i = 0; i < 10; ++i) { m->users[i].id = i + 1; snprintf(m->users[i].nickname, sizeof(m->users[i].nickname), "User%u", i + 1); }
    strcpy(m->users[1].nickname, "Боб");
    Key(&ui, m, VK_TAB, L'\t', 0); Key(&ui, m, VK_DOWN, 0, 0); Key(&ui, m, VK_RETURN, L'\r', 0);
    CHECK(m->selected == 2 && !m->contacts);
    strcpy(message.peer, "Боб"); strcpy(message.text, "Привет! Русский текст и English."); message.timestamp = 1700000000;
    Ui_Append(m, &message);
    Console_Layout(m, cells, 100, 30, &cursor);
    CHECK(Contains(cells, 100, 30, L"УЧАСТНИКИ")); CHECK(Contains(cells, 100, 30, L"Привет! Русский текст"));
    CHECK(cursor.X >= 0 && cursor.X < 100 && cursor.Y < 30);
    m->contacts = 1; m->focus = 9; Console_Layout(m, cells, 60, 22, &cursor);
    CHECK(Contains(cells, 60, 22, L"User10")); m->contacts = 0;
    memset(message.text, 'a', 4096); message.text[4096] = 0;
    for (i = 0; i < 270; ++i) { message.request = i + 1; Ui_Append(m, &message); }
    CHECK(m->message_count == UI_HISTORY_MAX && m->messages[0].request == 15);
    strcpy(m->messages[0].text, "FIRST-MARKER"); strcpy(m->messages[255].text, "LAST-MARKER");
    m->scroll = 0; Console_Layout(m, cells, 60, 22, &cursor); CHECK(Contains(cells, 60, 22, L"LAST-MARKER"));
    m->scroll = 65536; Console_Layout(m, cells, 60, 22, &cursor); CHECK(Contains(cells, 60, 22, L"FIRST-MARKER"));
    m->help = 1; m->scroll = 0; Console_Layout(m, cells, 100, 36, &cursor); CHECK(Contains(cells, 100, 36, L"/history clear"));
    m->help = 0;
    for (i = 1; i <= 320; ++i) { Console_Layout(m, cells, (int)i, 20, &cursor); CHECK(cursor.X < (SHORT)i && cursor.Y < 20); }
    CHECK(GetCurrentDirectoryW(950, directory));
    snprintf(pid, sizeof(pid), "\\history-contract-%lu", (unsigned long)GetCurrentProcessId());
    CHECK(MultiByteToWideChar(CP_UTF8, 0, pid, -1, suffix, 64)); wcscat(directory, suffix);
    CHECK(History_Init(&store, m, directory)); CHECK(History_Load(&store, m)); CHECK(History_Save(&store, m));
    strcpy(loaded->nickname, m->nickname); strcpy(loaded->endpoint, m->endpoint);
    CHECK(History_Load(&store, loaded)); CHECK(loaded->message_count == 256);
    CHECK(!strcmp(loaded->messages[0].peer, "Боб") && loaded->messages[0].delivery == UI_RESTORED);
    CHECK(!strcmp(loaded->messages[255].text, "LAST-MARKER"));
    snprintf(pid, sizeof(pid), ".%lu.tmp", (unsigned long)GetCurrentProcessId());
    CHECK(MultiByteToWideChar(CP_UTF8, 0, pid, -1, suffix, 64));
    wcscpy(blocked_temporary, store.path); wcscat(blocked_temporary, suffix);
    CHECK(CreateDirectoryW(blocked_temporary, NULL));
    strcpy(m->messages[255].text, "UNCOMMITTED"); CHECK(!History_Save(&store, m));
    CHECK(History_Load(&store, loaded) && !strcmp(loaded->messages[255].text, "LAST-MARKER"));
    CHECK(RemoveDirectoryW(blocked_temporary));
    strcpy(loaded->nickname, "Other"); CHECK(!History_Load(&store, loaded));
    strcpy(loaded->nickname, m->nickname);
    file = CreateFileW(store.path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE); CHECK(WriteFile(file, "BAD!", 4, &written, NULL)); CHECK(CloseHandle(file));
    CHECK(!History_Load(&store, loaded)); CHECK(loaded->message_count == 256);
    CHECK(History_Clear(&store)); CHECK(History_Clear(&store)); CHECK(RemoveDirectoryW(directory));
    if (argc == 2) Snapshot(argv[1], m, cells);
    free(cells); free(m); free(loaded);
    puts("PASS: UTF-8 editing, native key events, RU/EN, layout, wrap/scroll/resize, bounded WSH1 and corruption isolation");
    return 0;
}
