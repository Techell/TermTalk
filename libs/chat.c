#include "chat.h"
#include "branding.h"
#include "console_ui.h"
#include "history_store.h"
#include "client_transport.h"
#include "server_probe.h"
#include "file_transfer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    UiModel *model;
    ConsoleUi ui;
    HistoryStore history;
    int history_valid, finish, failed, keep_input;
    uint32_t request;
    ClientTransport *transport;
    ServerBook book;
    int book_valid, switch_server, exit_requested, open_menu;
    ServerAddress next_address, probe_address;
    ServerProbe *probe;
    DWORD probe_at, ping_at, ping_next;
    DWORD address_edited_at;
    char probe_input[UI_INPUT_MAX];
    uint32_t ping_request;
    int ping_manual;
    FileTransfer *files;
    FileBrowser *browser;
    char browse_path[BROWSER_PATH_MAX];
    int browse_pending, browse_drives, open_files;
    uint32_t file_peer, dialog_transfer;
} Application;

static int Same_Address(const ServerAddress *a, const ServerAddress *b)
{ return a->port == b->port && !strcmp(a->ip, b->ip); }

static void Book_Model(Application *app)
{
    memcpy(app->model->servers, app->book.entries, app->book.count * sizeof(ServerAddress));
    app->model->server_count = app->book.count;
    if (app->model->server_focus >= app->book.count) app->model->server_focus = 0;
}

static int Menu_Address(Application *app, ServerAddress *address)
{
    UiModel *m = app->model;
    if (*m->input) return ServerAddress_Parse(m->input, address);
    if (m->server_focus >= m->server_count) return 0;
    *address = m->servers[m->server_focus]; return 1;
}

static int Probe_Menu(Application *app, int refresh)
{
    UiModel *m = app->model;
    ServerAddress desired, active;
    ServerProbeResult result;
    DWORD now = GetTickCount();
    if (m->server_menu && strcmp(app->probe_input, m->input)) {
        strcpy(app->probe_input, m->input); app->address_edited_at = now;
        if (*m->input && !refresh) {
            ServerProbe_Destroy(app->probe); app->probe = NULL; app->probe_at = 0; m->probe_state = 0; return 1;
        }
    }
    /* Не подключаемся к промежуточным адресам во время набора последнего октета
       или порта. Явная F5 проверяет сразу, автоматическая проба ждёт паузу. */
    if (m->server_menu && *m->input && !refresh && now - app->address_edited_at < 350) return 0;
    if (!m->server_menu || !Menu_Address(app, &desired)) {
        ServerProbe_Destroy(app->probe); app->probe = NULL;
        if (m->probe_state) { m->probe_state = 0; return 1; }
        return 0;
    }
    if (m->online > 0 && ServerAddress_Parse(m->endpoint, &active) && Same_Address(&desired, &active)) {
        ServerProbe_Destroy(app->probe); app->probe = NULL;
        if (m->probe_state == 2 && m->probe_users == m->count && m->probe_latency == m->latency && m->probe_latency_valid == (m->latency_state == 1)) return 0;
        m->probe_capacity = m->capacity; m->probe_state = 2; m->probe_users = m->count; m->probe_latency = m->latency; m->probe_latency_valid = m->latency_state == 1; return 1;
    }
    if (refresh || !Same_Address(&desired, &app->probe_address)) {
        ServerProbe_Destroy(app->probe); app->probe = NULL; app->probe_address = desired; app->probe_at = 0; m->probe_state = 0;
    }
    if (app->probe && ServerProbe_Read(app->probe, &result)) {
        m->probe_capacity = result.capacity; m->probe_state = result.online ? 2 : 3; m->probe_users = result.users;
        m->probe_latency = result.latency; m->probe_latency_valid = result.online;
        ServerProbe_Destroy(app->probe); app->probe = NULL; app->probe_at = now; return 1;
    }
    if (!app->probe && (!app->probe_at || now - app->probe_at >= 3000)) {
        app->probe = ServerProbe_Start(&desired); app->probe_at = now; m->probe_state = app->probe ? 1 : 3; return 1;
    }
    return 0;
}

static void Notice(Application *app, const char *en, const char *ru)
{
    snprintf(app->model->notice, sizeof(app->model->notice), "%s", Ui_Text(app->model->language, en, ru));
    Console_Log(&app->ui, app->model->notice); Console_Log(&app->ui, "\n");
}

static int Post(Application *app, uint8_t type, const void *payload, size_t length)
{
    if (app->finish || app->request == UINT32_MAX
        || !Transport_Post(app->transport, type, app->request + 1, payload, length)) {
        Notice(app, "Outgoing queue full or session closing; command was not queued.",
            "Очередь заполнена или сеанс завершается; команда не поставлена."); app->keep_input = 1; return 0;
    }
    ++app->request; return 1;
}

static int File_Post(void *context, uint8_t type, const void *payload, size_t length, uint32_t *request)
{
    Application *app = context;
    if (app->finish || app->request == UINT32_MAX || !Transport_Post(app->transport, type, app->request + 1, payload, length)) return 0;
    *request = ++app->request; return 1;
}

static int File_Model(Application *app)
{
    UiModel *m = app->model;
    const FileInfo *info = FileTransfer_Info(app->files);
    int changed = m->file.status != info->status || m->file.id != info->id || m->file.bytes != info->bytes;
    if (info->status != m->file.status || (info->id != m->file.id && info->status == FILE_OFFERED)) {
        char note[256];
        if (info->status == FILE_OFFERED) snprintf(note, sizeof(note), Ui_Text(m->language,
            "File offer #%lu from %lu (%llu bytes). /accept or /reject",
            "Файл #%lu от %lu (%llu байт). /accept или /reject"),
            (unsigned long)info->id, (unsigned long)info->peer, (unsigned long long)info->total);
        else snprintf(note, sizeof(note), "%s #%lu (%llu/%llu)",
            info->status == FILE_COMPLETED ? (info->outgoing ? Ui_Text(m->language, "File completed", "Файл передан") : Ui_Text(m->language, "File saved", "Файл сохранён"))
            : info->status == FILE_FAILED ? Ui_Text(m->language, "File failed", "Ошибка передачи")
            : info->status == FILE_CANCELLED ? Ui_Text(m->language, "File cancelled", "Передача отменена")
            : info->status == FILE_REFUSED ? Ui_Text(m->language, "File declined", "Получатель отказался")
            : Ui_Text(m->language, "File transfer", "Передача файла"),
            (unsigned long)info->id, (unsigned long long)info->bytes, (unsigned long long)info->total);
        Notice(app, note, note);
    }
    m->file = *info; return changed;
}

static void Switch_Server(Application *app, const ServerAddress *address)
{
    if (!Post(app, PROTO_GOODBYE, NULL, 0)) return;
    app->next_address = *address; app->switch_server = app->finish = 1;
    Transport_Finish(app->transport);
}

static void Close_FileDialog(Application *app)
{
    Ui_FileDialog(app->model, 0); Browser_Cancel(app->browser);
    app->browse_pending = app->model->browser_loading = 0; app->dialog_transfer = 0;
}
static void Folder(Application *app, const char *path)
{
    UiModel *m = app->model;
    if (path && strlen(path) >= sizeof(app->browse_path)) { Notice(app, "Path is too long.", "Путь слишком длинный."); return; }
    Browser_Cancel(app->browser); app->browse_drives = path == NULL;
    if (path) strcpy(app->browse_path, path);
    app->browse_pending = m->browser_loading = 1; m->browser_focus = 0;
    m->input[0] = 0; m->cursor = 0;
}
static void Open_FileDialog(Application *app, int mode)
{
    UiModel *m = app->model;
    char directory[BROWSER_PATH_MAX];
    const FileInfo *file = FileTransfer_Info(app->files);
    if (mode == 1 && (FileTransfer_Busy(app->files) || !Ui_Peer(m, app->file_peer) || app->file_peer == m->own_id)) {
        Notice(app, "Select a participant first; only one file transfer can be active.", "Сначала выберите собеседника; одновременно доступна одна передача."); return;
    }
    if (mode == 3 && file->status != FILE_OFFERED) { Notice(app, "No incoming file offer.", "Нет входящего предложения файла."); return; }
    if (!Browser_ClientDirectory(mode == 3, directory, sizeof(directory))) { Notice(app, "Cannot determine the client folder.", "Не удалось определить папку клиента."); return; }
    if (m->server_menu) Ui_ServerMenu(m, 0);
    Ui_FileDialog(m, mode); m->destination_choice = 0;
    m->file_dialog_peer = mode == 1 ? app->file_peer : file->peer;
    if (mode == 3) { strcpy(m->download_directory, directory); app->dialog_transfer = file->id; }
    else Folder(app, directory);
    Notice(app, mode == 3 ? "Choose the default downloads folder or browse." : "Select a file. Enter opens a folder or sends a file.",
        mode == 3 ? "Выберите downloads рядом с клиентом или другую папку." : "Выберите файл. Enter открывает папку или отправляет файл.");
}
static int Browser_Tick(Application *app)
{
    UiModel *m = app->model;
    int changed = 0;
    if (Browser_Read(app->browser, &m->browser)) {
        m->browser_loading = app->browse_pending; m->browser_focus = 0; changed = 1;
        if (m->file_dialog && !app->browse_pending && m->browser.error) {
            char note[128]; snprintf(note, sizeof(note), Ui_Text(m->language, "Cannot read folder (Windows %lu). F6: drives.", "Не удалось прочитать папку (Windows %lu). F6: диски."), m->browser.error);
            Notice(app, note, note);
        } else if (m->file_dialog && m->browser.truncated)
            Notice(app, "First 512 entries shown. Type a full path for other files.", "Показаны первые 512 записей. Другие файлы доступны по полному пути.");
    }
    if (app->browse_pending) {
        int result = Browser_Request(app->browser, app->browse_drives ? NULL : app->browse_path);
        if (result) {
            app->browse_pending = 0; m->browser_loading = result > 0; changed = 1;
            if (result < 0) Notice(app, "Cannot open this path.", "Не удалось открыть этот путь.");
        }
    }
    if (m->file_dialog >= 2) {
        const FileInfo *file = FileTransfer_Info(app->files);
        if (file->status != FILE_OFFERED || file->id != app->dialog_transfer) {
            Close_FileDialog(app); Notice(app, "The file offer has ended.", "Предложение файла уже завершилось."); changed = 1;
        }
    }
    return changed;
}
static void Pick_Destination(Application *app)
{
    UiModel *m = app->model;
    char path[BROWSER_PATH_MAX];
    const FileInfo *file = FileTransfer_Info(app->files);
    const char *directory = m->file_dialog == 3 ? m->download_directory : m->browser.directory;
    if (file->status != FILE_OFFERED || file->id != app->dialog_transfer) { Close_FileDialog(app); return; }
    if ((m->file_dialog == 2 && (m->browser_loading || m->browser.error || !*directory))
        || !Browser_ReceivePath(directory, file->name, m->file_dialog == 3, path, sizeof(path))
        || !FileTransfer_Decide(app->files, file->id, path)) {
        Notice(app, "Cannot save here. Choose another folder or retry.", "Не удалось сохранить здесь. Выберите другую папку или повторите."); return;
    }
    Close_FileDialog(app);
    Notice(app, "Receiving the file into the chosen folder.", "Файл принимается в выбранную папку.");
}
static void File_Action(Application *app, UiAction action)
{
    UiModel *m = app->model;
    char path[BROWSER_PATH_MAX];
    if (action == UI_FILE_OPEN) { app->file_peer = m->selected; Open_FileDialog(app, 1); return; }
    if (!m->file_dialog) return;
    if (action == UI_FILE_CLOSE) { Close_FileDialog(app); return; }
    if (action == UI_FILE_BROWSE) {
        if (Browser_ClientDirectory(0, path, sizeof(path))) { Ui_FileDialog(m, 2); Folder(app, path); }
        return;
    }
    if (action == UI_FILE_PICK) { Pick_Destination(app); return; }
    if (action == UI_FILE_DRIVES) { Folder(app, NULL); return; }
    if (action == UI_FILE_REFRESH) { Folder(app, *m->browser.directory ? m->browser.directory : NULL); return; }
    if (action == UI_FILE_PARENT || (action == UI_FILE_ENTER && !*m->input && !m->browser_focus)) {
        if (Browser_Parent(m->browser.directory, path, sizeof(path))) Folder(app, *path ? path : NULL);
        return;
    }
    if (action != UI_FILE_ENTER || (m->browser_loading && !*m->input)) return;
    if (*m->input) {
        size_t n = strlen(m->input);
        const char *input = m->input;
        if (n >= 2 && input[0] == '"' && input[n - 1] == '"') { ++input; n -= 2; }
        if (n >= sizeof(path)) return;
        memcpy(path, input, n); path[n] = 0;
    } else if (!Browser_EntryPath(&m->browser, m->browser_focus - 1, path, sizeof(path))) return;
    {
        wchar_t wide[1024]; DWORD attributes;
        if (!MultiByteToWideChar(CP_UTF8, 0x8, path, -1, wide, 1024)) return;
        attributes = GetFileAttributesW(wide);
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY)) { Folder(app, path); return; }
        if (m->file_dialog == 2) { Notice(app, "Choose a folder; F7 saves into the current folder.", "Выберите папку; F7 сохраняет в текущую папку."); return; }
    }
    if (!Ui_Peer(m, app->file_peer) || !FileTransfer_Send(app->files, app->file_peer, path)) {
        Notice(app, "Cannot send this file; check the path, recipient and transfer state.", "Не удалось отправить файл: проверьте путь, собеседника и состояние передачи."); return;
    }
    Close_FileDialog(app); Notice(app, "File offered to the recipient.", "Файл предложен собеседнику.");
}

static int Receive(Application *app, const ClientEvent *event)
{
    UiModel *m = app->model;
    const unsigned char *p = event->payload;
    char output[PROTOCOL_TEXT_MAX + 300];
    unsigned i;
    if (event->type == PROTO_ERROR) {
        ProtocolFrame frame = {event->type, event->request, event->payload, event->length};
        FileTransfer_Frame(app->files, &frame);
        snprintf(output, sizeof(output), Ui_Text(m->language, "Server error %u (request %lu): %.*s",
            "Ошибка сервера %u (запрос %lu): %.*s"), (unsigned)Protocol_Read16(p),
            (unsigned long)event->request, (int)event->length - 4, (const char *)p + 4);
        if (!m->online && Protocol_Read16(p) == PROTO_NICK_IN_USE)
            snprintf(output, sizeof(output), "%s", Ui_Text(m->language,
                "Nickname is already in use. F7 in the server menu changes it.",
                "Никнейм занят. Нажмите F7 в меню серверов, чтобы изменить его."));
        Notice(app, output, output);
        for (i = 0; i < m->message_count; ++i)
            if (m->messages[i].request == event->request && m->messages[i].delivery == UI_PENDING) m->messages[i].delivery = UI_FAILED;
        return m->online ? 1 : 0;
    }
    if (event->type == PROTO_WELCOME) {
        if (m->online || event->request != 1) return 0;
        m->own_id = Protocol_Read32(p); m->online = 1;
        snprintf(output, sizeof(output), Ui_Text(m->language, "Registered as %s (id=%lu).", "Вход выполнен: %s (id=%lu)."),
            m->nickname, (unsigned long)m->own_id); Notice(app, output, output);
        {
            ServerAddress address;
            if (ServerAddress_Parse(m->endpoint, &address)) {
                ServerBook_Add(&app->book, &address); Book_Model(app);
                if (m->saving && app->book_valid && !ServerBook_Save(&app->book))
                    Notice(app, "Cannot save the server list.", "Не удалось сохранить список серверов.");
            }
        }
        return 1;
    }
    if (!m->online) return 0;
    switch (event->type) {
    case PROTO_STATUS_RESPONSE:
        m->capacity = Protocol_Read16(p + 2); m->file_limit = Protocol_Read64(p + 4);
        if (!event->request) return 1; /* Метаданные после WELCOME не являются замером RTT. */
        if (event->request == app->ping_request) {
            m->latency = GetTickCount() - app->ping_at; m->latency_state = 1; app->ping_request = 0; app->ping_next = GetTickCount() + 3000;
        }
        snprintf(output, sizeof(output), Ui_Text(m->language, "Server online: %u/%u users, ping %u ms; file limit %llu bytes.", "Сервер онлайн: %u/%u участников, пинг %u мс; предел файла %llu байт."),
            (unsigned)Protocol_Read16(p), (unsigned)Protocol_Read16(p + 2), m->latency, (unsigned long long)m->file_limit);
        Notice(app, output, output); return 1;
    case PROTO_FILE_OFFER_EVENT: case PROTO_FILE_CREATED: case PROTO_FILE_DECISION_EVENT:
    case PROTO_FILE_DATA: case PROTO_FILE_ACK: case PROTO_FILE_END: case PROTO_FILE_DONE: case PROTO_FILE_CANCEL: {
        ProtocolFrame frame = {event->type, event->request, event->payload, event->length};
        if (event->type != PROTO_FILE_CREATED && event->request) return 0;
        FileTransfer_Frame(app->files, &frame); return 1;
    }
    case PROTO_LIST_RESPONSE: {
        ProtocolParticipant users[PROTOCOL_USERS_MAX];
        unsigned unread[PROTOCOL_USERS_MAX] = {0}, count = Protocol_Read16(p);
        size_t at = 2;
        int own = 0, selected = 0;
        for (i = 0; i < count; ++i) {
            unsigned j;
            if (!Protocol_ReadParticipant(p, event->length, &at, &users[i])) return 0;
            if (users[i].id == m->own_id) own = 1;
            if (users[i].id == m->selected) selected = 1;
            for (j = 0; j < m->count; ++j) if (users[i].id == m->users[j].id) unread[i] = m->unread[j];
        }
        if (!own) return 0;
        memcpy(m->users, users, count * sizeof(*users)); memcpy(m->unread, unread, sizeof(unread)); m->count = count;
        if (m->focus >= count) m->focus = 0;
        if (!selected && m->selected) { m->selected = 0; Notice(app, "Participant disconnected. Choose another recipient.", "Собеседник отключился. Выберите другого участника."); }
        snprintf(output, sizeof(output), Ui_Text(m->language, "Participants (%u):\n", "Участники (%u):\n"), count); Console_Log(&app->ui, output);
        for (i = 0; i < count; ++i) {
            snprintf(output, sizeof(output), "%lu | %s | %u.%u.%u.%u:%u | %s\n", (unsigned long)users[i].id,
                users[i].nickname, users[i].ipv4[0], users[i].ipv4[1], users[i].ipv4[2], users[i].ipv4[3], users[i].port,
                Ui_Text(m->language, "online", "в сети")); Console_Log(&app->ui, output);
        }
        return 1;
    }
    case PROTO_CHAT_EVENT: {
        UiMessage message = {0};
        const ProtocolParticipant *peer;
        message.id = Protocol_Read32(p); peer = Ui_Peer(m, message.id);
        if (peer) strcpy(message.peer, peer->nickname);
        else snprintf(message.peer, sizeof(message.peer), "#%lu", (unsigned long)message.id);
        memcpy(message.text, p + 8, event->length - 8); message.timestamp = time(NULL);
        Ui_Append(m, &message);
        for (i = 0; i < m->count; ++i) if (m->users[i].id == message.id && m->selected != message.id) ++m->unread[i];
        snprintf(output, sizeof(output), Ui_Text(m->language, "From %lu (%s): %s\n", "От %lu (%s): %s\n"),
            (unsigned long)message.id, message.peer, message.text); Console_Log(&app->ui, output); return 1;
    }
    case PROTO_ACCEPTED:
        for (i = 0; i < m->message_count; ++i)
            if (m->messages[i].outgoing && m->messages[i].request == event->request) m->messages[i].delivery = UI_ACCEPTED;
        snprintf(output, sizeof(output), Ui_Text(m->language,
            "Accepted request %lu for %lu; delivery is not confirmed.", "Принят запрос %lu для %lu; доставка не подтверждена."),
            (unsigned long)event->request, (unsigned long)Protocol_Read32(p)); Notice(app, output, output); return 1;
    case PROTO_PONG:
        if (event->request == app->ping_request) {
            m->latency = GetTickCount() - app->ping_at; m->latency_state = 1; app->ping_request = 0;
            app->ping_next = GetTickCount() + 3000;
        }
        if (!app->ping_manual && app->ui.interactive) return 1;
        app->ping_manual = 0;
        snprintf(output, sizeof(output), Ui_Text(m->language, "PONG — server responded (%u ms).", "PONG — сервер ответил (%u мс)."), m->latency);
        Notice(app, output, output); return 1;
    default: return 0;
    }
}

static int Parse_ID(const char *text, uint32_t *id, const char **end)
{
    uint32_t value = 0;
    const char *p = text;
    if (*p < '0' || *p > '9') return 0;
    while (*p >= '0' && *p <= '9') {
        unsigned digit = (unsigned)(*p++ - '0');
        if (value > (UINT32_MAX - digit) / 10) return 0;
        value = value * 10 + digit;
    }
    if (!value) return 0;
    *id = value; *end = p; return 1;
}

static int Save(Application *app)
{
    int saved = app->history_valid && History_Save(&app->history, app->model);
    if (saved) Notice(app, "History saved.", "История сохранена.");
    else Notice(app, "Cannot save history; existing journal was preserved.", "Не удалось сохранить историю; прежний журнал сохранён.");
    return saved;
}

static void Command(Application *app)
{
    UiModel *m = app->model;
    const char *text = m->input, *end;
    uint32_t id = m->selected;
    if (!strcmp(text, "/help")) {
        m->help = 1; m->scroll = 0; Console_Log(&app->ui, Ui_Help(m->language)); Console_Log(&app->ui, "\n"); return;
    }
    if (!strcmp(text, "/lang en") || !strcmp(text, "/lang ru")) {
        m->language = !strcmp(text + 6, "ru") ? UI_RU : UI_EN;
        Notice(app, "Language: English.", "Язык: Русский."); return;
    }
    if (!strcmp(text, "/servers")) {
        unsigned i;
        char note[100];
        for (i = 0; i < app->book.count; ++i) {
            snprintf(note, sizeof(note), "%u | %s:%u\n", i + 1, app->book.entries[i].ip, app->book.entries[i].port);
            Console_Log(&app->ui, note);
        }
        if (app->ui.interactive) app->open_menu = 1;
        else Notice(app, "Use /connect IPv4:port to switch.", "Для смены: /connect IPv4:порт.");
        return;
    }
    if (!strncmp(text, "/connect ", 9)) {
        ServerAddress address;
        if (!ServerAddress_Parse(text + 9, &address)) { Notice(app, "Usage: /connect IPv4:port", "Команда: /connect IPv4:порт"); app->keep_input = 1; }
        else Switch_Server(app, &address);
        return;
    }
    if (!strncmp(text, "/history ", 9)) {
        const char *op = text + 9;
        if (!strcmp(op, "off")) { m->saving = 0; Notice(app, "History saving disabled. Existing file remains.", "Сохранение отключено. Существующий файл остаётся."); }
        else if (!strcmp(op, "on")) {
            m->saving = app->history_valid;
            Notice(app, m->saving ? "History saving enabled." : "History directory unavailable.",
                m->saving ? "Сохранение включено." : "Каталог истории недоступен.");
        } else if (!strcmp(op, "save")) {
            if (m->saving) Save(app);
            else Notice(app, "Enable saving with /history on first.", "Сначала включите сохранение: /history on.");
        } else if (!strcmp(op, "clear")) {
            if (app->history_valid && History_Clear(&app->history)) {
                m->message_count = 0; m->scroll = 0;
                Notice(app, "This profile's journal was deleted.", "Журнал этого профиля удалён.");
            } else Notice(app, "Cannot delete history.", "Не удалось удалить историю.");
        } else Notice(app, "Usage: /history on|off|save|clear", "Команда: /history on|off|save|clear");
        return;
    }
    if (!strcmp(text, "/disconnect")) {
        if (Post(app, PROTO_GOODBYE, NULL, 0)) { app->finish = 1; Transport_Finish(app->transport); }
        return;
    }
    if (!strcmp(text, "/quit")) {
        if (Post(app, PROTO_GOODBYE, NULL, 0)) { app->finish = app->exit_requested = 1; Transport_Finish(app->transport); }
        return;
    }
    if (!m->online) {
        Notice(app, "Waiting for server registration; your input was retained.", "Ожидается регистрация на сервере; ввод сохранён.");
        app->keep_input = 1; return;
    }
    if (!strcmp(text, "/list")) { Post(app, PROTO_LIST_REQUEST, NULL, 0); return; }
    if (!strcmp(text, "/ping") || !strcmp(text, "/status")) {
        if (Post(app, !strcmp(text, "/ping") ? PROTO_PING : PROTO_STATUS_REQUEST, NULL, 0)) {
            app->ping_at = GetTickCount(); app->ping_request = app->request; app->ping_manual = 1;
        }
        return;
    }
    if (!strcmp(text, "/cancel")) {
        if (FileTransfer_Busy(app->files)) FileTransfer_Cancel(app->files, 1);
        else Notice(app, "No active file transfer.", "Нет активной передачи файла.");
        return;
    }
    if (!strcmp(text, "/sendfile") || !strncmp(text, "/sendfile ", 10)) {
        const char *path = text + 9;
        uint32_t parsed_id;
        while (*path == ' ') ++path;
        if (*path && Parse_ID(path, &parsed_id, &end) && (!*end || *end == ' ')) {
            id = parsed_id; path = end; while (*path == ' ') ++path;
        }
        if (!id || id == m->own_id || !Ui_Peer(m, id)) {
            Notice(app, "Select a recipient or use /sendfile ID [path].", "Выберите собеседника или введите /sendfile ID [путь]."); return;
        }
        if (!*path) {
            if (app->ui.interactive) { app->file_peer = id; app->open_files = 1; }
            else Notice(app, "Usage: /sendfile ID full-path", "Команда: /sendfile ID полный-путь");
        } else if (!FileTransfer_Send(app->files, id, path)) {
            Notice(app, "Cannot use file: transfer busy, invalid path/ID, existing destination or size over 1 TiB.",
                "Не удалось: передача занята, неверный путь/ID, файл уже существует или размер больше 1 ТиБ."); app->keep_input = 1;
        }
        return;
    }
    if (!strcmp(text, "/accept") || !strncmp(text, "/accept ", 8) || !strcmp(text, "/reject") || !strncmp(text, "/reject ", 8)) {
        int reject = !strncmp(text, "/reject", 7);
        uint32_t parsed_id;
        const char *path = text + 7;
        const FileInfo *file = FileTransfer_Info(app->files);
        id = file->id; while (*path == ' ') ++path;
        if (*path && Parse_ID(path, &parsed_id, &end) && (!*end || *end == ' ')) { id = parsed_id; path = end; while (*path == ' ') ++path; }
        if (file->status != FILE_OFFERED || file->id != id || (reject && *path)) {
            Notice(app, "No matching file offer. Use /accept or /reject.", "Нет такого предложения файла. Используйте /accept или /reject."); return;
        }
        if (reject) FileTransfer_Decide(app->files, id, NULL);
        else if (*path) {
            if (!FileTransfer_Decide(app->files, id, path)) {
                Notice(app, "Cannot use file: existing destination or invalid path.", "Не удалось: файл уже существует или путь недопустим."); app->keep_input = 1;
            }
        } else if (app->ui.interactive) app->open_files = 3;
        else {
            char directory[BROWSER_PATH_MAX], destination[BROWSER_PATH_MAX];
            if (!Browser_ClientDirectory(1, directory, sizeof(directory))
                || !Browser_ReceivePath(directory, file->name, 1, destination, sizeof(destination))
                || !FileTransfer_Decide(app->files, id, destination))
                Notice(app, "Cannot save into the default downloads folder.", "Не удалось сохранить в стандартную папку downloads.");
        }
        return;
    }
    if (!strncmp(text, "/to ", 4)) {
        if (Parse_ID(text + 4, &id, &end) && !*end && id != m->own_id && Ui_Peer(m, id)) {
            char note[100];
            unsigned i;
            m->selected = id; m->scroll = 0;
            for (i = 0; i < m->count; ++i) if (m->users[i].id == id) m->unread[i] = 0;
            snprintf(note, sizeof(note), Ui_Text(m->language, "Selected recipient %lu.", "Выбран собеседник %lu."), (unsigned long)id);
            Notice(app, note, note);
        } else Notice(app, "Choose an online participant ID from /list.", "Выберите ID участника в сети из /list.");
        return;
    }
    if (!strncmp(text, "/msg ", 5)) {
        if (!Parse_ID(text + 5, &id, &end) || *end != ' ' || !end[1]) {
            Notice(app, "Usage: /msg ID text", "Команда: /msg ID текст"); return;
        }
        text = end + 1;
    } else if (*text == '/') { Notice(app, "Unknown command. Use /help.", "Неизвестная команда. Откройте /help."); return; }
    if (!*text) return;
    if (!id) { Notice(app, "Select a recipient with /to ID or use /msg ID text.", "Выберите собеседника: /to ID или /msg ID текст."); return; }
    {
        unsigned char payload[4 + PROTOCOL_TEXT_MAX];
        size_t n = strlen(text);
        UiMessage message = {0};
        const ProtocolParticipant *peer = Ui_Peer(m, id);
        if (n > PROTOCOL_TEXT_MAX || !Protocol_ValidText((const unsigned char *)text, n, 0)) {
            Notice(app, "Text must contain 1..4096 valid UTF-8 bytes.", "Текст должен содержать 1..4096 корректных байтов UTF-8."); return;
        }
        Protocol_Write32(payload, id); memcpy(payload + 4, text, n);
        if (!Post(app, PROTO_CHAT_SEND, payload, n + 4)) return;
        message.outgoing = 1; message.id = id; message.request = app->request; message.timestamp = time(NULL);
        if (peer) strcpy(message.peer, peer->nickname); else snprintf(message.peer, sizeof(message.peer), "#%lu", (unsigned long)id);
        strcpy(message.text, text); Ui_Append(m, &message); m->scroll = 0;
    }
}

static int Login(Application *app)
{
    UiModel *m = app->model;
    char draft[UI_INPUT_MAX];
    size_t cursor = m->cursor;
    int online = m->online, menu = m->server_menu, result = 1;
    /* Форма не открывает сеть и не подменяет черновик адреса. Старый профиль
       уже сохранён при закрытии Session; следующий HELLO возьмёт новое имя. */
    strcpy(draft, m->input); strcpy(m->input, m->nickname); m->cursor = strlen(m->input);
    m->online = -1; m->server_menu = m->help = m->contacts = m->scroll = 0;
    while (1) {
        UiAction action;
        if (!Console_Draw(&app->ui, m)) { result = -1; break; }
        action = Console_Poll(&app->ui, m);
        if (action == UI_INPUT_ERROR) { result = -1; break; }
        if (action == UI_QUIT || action == UI_EOF) { result = 0; break; }
        if (action == UI_NICKNAME_CANCEL) break;
        if (action == UI_SUBMIT) {
            size_t n = strlen(m->input);
            if (n && n <= PROTOCOL_NICK_MAX && Protocol_ValidText((const unsigned char *)m->input, n, 1)) {
                strcpy(m->nickname, m->input); Notice(app, "Nickname saved. Choose a server to connect.", "Никнейм сохранён. Выберите сервер для подключения."); break;
            }
            Notice(app, "Nickname must contain 1..32 UTF-8 bytes, without control characters.", "Никнейм: 1..32 байта UTF-8, без управляющих символов.");
        }
        Sleep(10);
    }
    strcpy(m->input, draft); m->cursor = cursor; m->online = online; m->server_menu = menu;
    return result;
}

static int Choose_Server(Application *app, ServerAddress *address)
{
    UiModel *m = app->model;
    Ui_ServerMenu(m, 1); app->probe_at = 0;
    while (m->server_menu) {
        UiAction action;
        Probe_Menu(app, 0);
        if (!Console_Draw(&app->ui, m)) return -1;
        action = Console_Poll(&app->ui, m);
        if (action == UI_QUIT || action == UI_EOF) return 0;
        if (action == UI_INPUT_ERROR) return -1;
        if (action == UI_NICKNAME) {
            int login = Login(app);
            if (login <= 0) return login;
        }
        if (action == UI_SERVER_CHECK) Probe_Menu(app, 1);
        if (action == UI_SERVER_CONNECT) {
            if (Menu_Address(app, address)) {
                Ui_ServerMenu(m, 0); ServerProbe_Destroy(app->probe); app->probe = NULL; return 1;
            }
            Notice(app, "Enter a valid IPv4:port, or clear input and select the default.", "Введите IPv4:порт или очистите строку и выберите стандартный сервер.");
        }
        Sleep(10);
    }
    return 0;
}

static int Session(Application *app, const ServerAddress *address, const ChatOptions *options)
{
    Client client = CLIENT_INITIALIZER;
    UiModel *m = app->model;
    int error = 0, dirty = 1, save_failed = 0;
    unsigned char hello[2 + PROTOCOL_NICK_MAX];
    app->failed = app->finish = app->switch_server = app->request = 0;
    Ui_ClearCommands(m);
    app->ping_request = 0; app->ping_next = GetTickCount() + 3000;
    m->online = -2; m->own_id = m->selected = m->count = m->focus = m->message_count = 0;
    m->latency_state = 0; m->latency = 0; m->capacity = 0; m->file_limit = 0; app->ping_manual = 0; m->contacts = m->help = m->scroll = 0;
    memset(m->unread, 0, sizeof(m->unread)); memset(&m->file, 0, sizeof(m->file));
    m->input[0] = m->chat_draft[0] = 0; m->cursor = m->draft_cursor = 0;
    if (m->server_menu) Ui_ServerMenu(m, 0);
    snprintf(m->endpoint, sizeof(m->endpoint), "%s:%u", address->ip, address->port);
    app->history_valid = History_Init(&app->history, m, options->history_directory);
    if (m->saving && (!app->history_valid || !History_Load(&app->history, m))) {
        m->saving = 0; Notice(app, "History unavailable or damaged; saving disabled.", "История недоступна или повреждена; сохранение отключено.");
    }
    if (!Console_Draw(&app->ui, m)) { app->failed = 1; goto cleanup; }
    if (Client_ConnectTimed(&client, address->ip, address->port, 1500, NULL)) {
        error = WSAGetLastError();
        snprintf(m->notice, sizeof(m->notice), Ui_Text(m->language, "Connect failed: %s (%d)", "Ошибка подключения: %s (%d)"), Network_ErrorText(error), error);
        app->failed = 1; goto cleanup;
    }
    m->online = 0;
    Notice(app, "TermTalk | Connected. /help for commands.", "TermTalk | Подключено. Команды: /help.");
    app->transport = Transport_Create(&client); app->files = FileTransfer_Create(File_Post, app);
    if (!app->transport || !app->files) { app->failed = 1; Notice(app, "Cannot create session resources.", "Не удалось создать ресурсы сеанса."); goto cleanup; }
    Protocol_Write16(hello, (uint16_t)strlen(m->nickname)); memcpy(hello + 2, m->nickname, strlen(m->nickname));
    if (!Post(app, PROTO_HELLO, hello, strlen(m->nickname) + 2)) { app->failed = 1; goto cleanup; }
    while (1) {
        ClientEvent event;
        UiAction action = UI_IDLE;
        int done = Transport_Done(app->transport, &error);
        while (Transport_Pop(app->transport, &event)) {
            if (!Receive(app, &event)) {
                if (event.type != PROTO_ERROR) Notice(app, "Invalid server protocol state or frame.", "Неверное состояние или кадр протокола сервера.");
                /* Сервер вправе закрыть соединение после отказа HELLO. EOF до
                   WELCOME не должен затереть понятную причину отказа ошибкой ОС. */
                else error = 0;
                app->failed = 1; break;
            }
            dirty = 1;
        }
        if (app->failed || done) { if (error) app->failed = 1; break; }
        {
            unsigned input_budget = app->ui.interactive ? 64 : 1;
            /* Пакетный разбор клавиш не замедляет вставку большого текста до
               одного символа за кадр; ограничение оставляет время сетевым событиям.
               Перенаправленный stdin не потребляется до WELCOME. */
            while (input_budget-- && !app->finish && (m->online || app->ui.interactive)) {
                action = Console_Poll(&app->ui, m);
                if (action == UI_IDLE) break;
                dirty = 1;
                if (action == UI_INPUT_ERROR) {
                    app->failed = 1; Notice(app, "Input line exceeds the limit, contains NUL or cannot be read.", "Строка превышает предел, содержит NUL или не может быть прочитана."); break;
                }
                if (action == UI_SUBMIT) {
                    if (m->online > 0) Ui_RememberCommand(m, m->input);
                    app->keep_input = 0; Command(app);
                    if (app->keep_input) break;
                    m->input[0] = 0; m->cursor = 0;
                    if (app->open_menu) { app->open_menu = 0; Ui_ServerMenu(m, 1); }
                    if (app->open_files) { int mode = app->open_files; app->open_files = 0; Open_FileDialog(app, mode); }
                }
                if (action >= UI_FILE_OPEN && action <= UI_FILE_BROWSE) File_Action(app, action);
                if (action == UI_SERVER_CHECK) { Probe_Menu(app, 1); }
                if (action == UI_DISCONNECT && Post(app, PROTO_GOODBYE, NULL, 0)) { app->finish = 1; Transport_Finish(app->transport); }
                if (action == UI_SERVER_CONNECT) {
                    ServerAddress next;
                    if (Menu_Address(app, &next)) Switch_Server(app, &next);
                    else Notice(app, "Enter a valid IPv4:port.", "Введите корректные IPv4:порт.");
                }
                if (action == UI_REFRESH && m->online) Post(app, PROTO_LIST_REQUEST, NULL, 0);
                if (action == UI_QUIT) {
                    if (Post(app, PROTO_GOODBYE, NULL, 0)) { app->finish = app->exit_requested = 1; Transport_Finish(app->transport); }
                }
                if (action == UI_EOF) { app->finish = app->exit_requested = 1; Transport_Finish(app->transport); }
            }
        }
        if (app->failed) break;
        if (!app->finish) {
            FileTransfer_Tick(app->files);
            if (File_Model(app)) dirty = 1;
            if (Browser_Tick(app)) dirty = 1;
            if (app->ui.interactive && m->online && !app->ping_request && (LONG)(GetTickCount() - app->ping_next) >= 0) {
                uint32_t request;
                if (File_Post(app, PROTO_PING, NULL, 0, &request)) { app->ping_request = request; app->ping_at = GetTickCount(); app->ping_manual = 0; }
            }
            if (app->ping_request && GetTickCount() - app->ping_at >= 5000) {
                app->ping_request = 0; app->ping_next = GetTickCount() + 3000; m->latency_state = -1; dirty = 1;
            }
        }
        if (Probe_Menu(app, 0)) dirty = 1;
        if (dirty && !Console_Draw(&app->ui, m)) {
            Notice(app, "Cannot update console screen.", "Не удалось обновить экран консоли."); app->failed = 1; break;
        }
        dirty = 0; Sleep(app->ui.interactive ? 10 : 1);
    }
    if (error) {
        char note[180]; snprintf(note, sizeof(note), Ui_Text(m->language, "Session error: %s (%d).", "Ошибка сеанса: %s (%d)."), Network_ErrorText(error), error);
        Notice(app, note, note);
    }
cleanup:
    if (m->file_dialog) Close_FileDialog(app);
    ServerProbe_Destroy(app->probe); app->probe = NULL;
    Transport_Destroy(app->transport); app->transport = NULL;
    FileTransfer_Destroy(app->files); app->files = NULL;
    Client_Close(&client);
    if (app->failed && !app->ui.interactive) { Console_Report(m->notice); Console_Report("\n"); }
    if (m->saving && m->own_id) save_failed = !Save(app);
    if (save_failed) { Console_Report(m->notice); Console_Report("\n"); }
    m->online = -3;
    return app->failed ? -1 : 0;
}

int Chat_Run(const char *address, unsigned short port, const char *nickname, const ChatOptions *options)
{
    Application app = {0};
    UiModel *m = calloc(1, sizeof(*m));
    ServerAddress selected;
    int network_ready = 0, result = 0, book_ready;
    if (!m) return -1;
    app.model = m; m->language = options->language; m->saving = options->save_history; m->online = -3;
    if (!Console_Open(&app.ui)) { free(m); return -1; }
    Console_Log(&app.ui, TERMTALK_BANNER);
    app.browser = Browser_Create();
    if (!app.browser) { Console_Close(&app.ui); free(m); return -1; }
    result = Network_Initialize();
    if (result) { result = -1; goto done; }
    network_ready = 1;
    book_ready = ServerBook_Init(&app.book, options->history_directory); app.book_valid = book_ready;
    if (book_ready && m->saving && !ServerBook_Load(&app.book)) {
        app.book_valid = 0; Notice(&app, "Saved server list is damaged; automatic saving disabled.", "Список серверов повреждён; его автоматическое сохранение отключено.");
    }
    Book_Model(&app);
    if (strlen(address) >= sizeof(selected.ip)) { Console_Report("Connect failed: invalid IPv4\n"); result = -1; goto done; }
    strcpy(selected.ip, address); selected.port = port;
    snprintf(m->endpoint, sizeof(m->endpoint), "%s:%u", address, port);
    if (nickname) strcpy(m->nickname, nickname);
    if (app.ui.interactive && options->choose_server) {
        int chosen = Choose_Server(&app, &selected);
        if (chosen <= 0) { result = chosen < 0 ? -1 : 0; goto done; }
        snprintf(m->endpoint, sizeof(m->endpoint), "%s:%u", selected.ip, selected.port);
    }
    if (!*m->nickname && app.ui.interactive) {
        int login = Login(&app);
        if (login <= 0) { result = login < 0 ? -1 : 0; goto done; }
    } else if (!*m->nickname) snprintf(m->nickname, sizeof(m->nickname), "Guest-%lu", (unsigned long)GetCurrentProcessId());
    while (1) {
        result = Session(&app, &selected, options);
        if (app.switch_server && !app.exit_requested) { selected = app.next_address; continue; }
        if (!app.ui.interactive || app.exit_requested) break;
        /* Соединение закончено, но приложение остаётся доступным для другого
           адреса. История старого профиля уже сохранена, его сокет закрыт. */
        {
            int chosen = Choose_Server(&app, &selected);
            if (chosen <= 0) { result = chosen < 0 ? -1 : 0; break; }
        }
    }
done:
    ServerProbe_Destroy(app.probe);
    Browser_Destroy(app.browser);
    Console_Close(&app.ui);
    if (network_ready) Network_Cleanup();
    free(m); return result;
}
