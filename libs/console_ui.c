#include "console_ui.h"
#include "branding.h"
#include <winerror.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_TEXT 7
#define C_BRIGHT 15
#define C_MUTED 8
#define C_ACCENT 14
#define C_GREEN 10
#define C_YELLOW 6
#define C_RED 12
#define C_SELECT (15 | (1 << 4))

/* Старый MinGW SDK не объявляет Vista+ EX API. Не повышаем требования к SDK:
   узнаём наличие функций во время запуска; меняем палитру только нашего буфера. */
typedef struct {
    ULONG cbSize;
    COORD size, cursor;
    WORD attributes;
    SMALL_RECT window;
    COORD maximum;
    WORD popup;
    BOOL fullscreen;
    COLORREF colors[16];
} PaletteInfo;
typedef BOOL (WINAPI *PaletteFunction)(HANDLE, PaletteInfo *);

static void Palette(HANDLE screen)
{
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    FARPROC address = GetProcAddress(kernel, "GetConsoleScreenBufferInfoEx");
    PaletteFunction get_info = NULL, set_info = NULL;
    PaletteInfo info;
    memcpy(&get_info, &address, sizeof(get_info));
    address = GetProcAddress(kernel, "SetConsoleScreenBufferInfoEx"); memcpy(&set_info, &address, sizeof(set_info));
    memset(&info, 0, sizeof(info)); info.cbSize = sizeof(info);
    if (!get_info || !set_info || !get_info(screen, &info)) return;
    info.colors[0] = RGB(18, 19, 23); info.colors[1] = RGB(46, 53, 65);
    info.colors[7] = RGB(181, 185, 194); info.colors[8] = RGB(118, 127, 143);
    info.colors[10] = RGB(124, 197, 157); info.colors[14] = RGB(222, 164, 128);
    info.colors[6] = RGB(230, 193, 91); info.colors[12] = RGB(232, 116, 116);
    info.colors[15] = RGB(239, 234, 226);
    /* EX setter ожидает нижнюю/правую границы в форме исключающего предела. */
    ++info.window.Right; ++info.window.Bottom; set_info(screen, &info);
}

/* Пользовательский текст становится данными CHAR_INFO, а не управляющими
   последовательностями терминала. Вывод принадлежит только основному потоку. */
static unsigned Codepoint(const char **text)
{
    const unsigned char *p = (const unsigned char *)*text;
    unsigned c = *p++;
    if (c >= 0xf0) { c &= 7; c = (c << 6) | (*p++ & 63); c = (c << 6) | (*p++ & 63); c = (c << 6) | (*p++ & 63); }
    else if (c >= 0xe0) { c &= 15; c = (c << 6) | (*p++ & 63); c = (c << 6) | (*p++ & 63); }
    else if (c >= 0xc0) { c &= 31; c = (c << 6) | (*p++ & 63); }
    *text = (const char *)p; return c;
}

static void Cell(CHAR_INFO *cells, int w, int h, int x, int y, unsigned c, WORD color)
{
    /* Размеры положительны по контракту Layout; отрицательные координаты после
       unsigned-преобразования также выходят за границы. */
    if ((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    cells[y * w + x].Char.UnicodeChar = (WCHAR)c; cells[y * w + x].Attributes = color;
}

static int Text(CHAR_INFO *cells, int w, int h, int x, int y, int end, const char *text, WORD color)
{
    while (*text && x < end) {
        unsigned c = Codepoint(&text);
        if (c > 0xffff) {
            if (x + 1 >= end) break;
            c -= 0x10000; Cell(cells, w, h, x++, y, 0xd800 + (c >> 10), color); c = 0xdc00 + (c & 1023);
        }
        Cell(cells, w, h, x++, y, c >= 32 ? c : ' ', color);
    }
    return x;
}
static void Rule(CHAR_INFO *cells, int w, int h, int x, int y, int end)
{ while (x < end) Cell(cells, w, h, x++, y, 0x2500, C_MUTED); }

typedef struct { CHAR_INFO *cells; int width, height, left, right, top, bottom, line, start; } Lines;

static void Wrapped(Lines *out, const char *text, WORD color)
{
    int x = out->left, word_start = 1;
    do {
        unsigned c;
        /* Переносим целое слово, когда оно помещается в строку. Длинное слово
           всё равно режется по Unicode-скалярам и не требует временного буфера. */
        if (word_start && *text && *text != ' ' && *text != '\t' && *text != '\n') {
            const char *look = text;
            int word = 0;
            while (*look && *look != ' ' && *look != '\t' && *look != '\n' && word <= out->right - out->left)
                word += Codepoint(&look) > 0xffff ? 2 : 1;
            if (x > out->left && word <= out->right - out->left && x + word > out->right) { ++out->line; x = out->left; }
        }
        c = *text ? Codepoint(&text) : 0;
        word_start = c == ' ' || c == '\t' || c == '\n';
        int slots = c > 0xffff ? 2 : 1, k;
        if (c == '\n' || !c || x + slots > out->right) {
            ++out->line; x = out->left;
            if (c == '\n') continue;
            if (!c) break;
        }
        if (c == '\t') slots = 4;
        for (k = 0; k < slots; ++k) {
            unsigned glyph = c;
            int y;
            if (c == '\t') glyph = ' ';
            else if (c > 0xffff) glyph = k ? 0xdc00 + ((c - 0x10000) & 1023) : 0xd800 + ((c - 0x10000) >> 10);
            if (x >= out->right) { ++out->line; x = out->left; }
            y = out->top + out->line - out->start;
            if (out->cells && y >= out->top && y < out->bottom) Cell(out->cells, out->width, out->height, x, y, glyph, color);
            ++x;
        }
    } while (1);
}

static void Conversation(Lines *out, const UiModel *m)
{
    unsigned i;
    const ProtocolParticipant *peer = Ui_Peer(m, m->selected);
    for (i = 0; i < m->message_count; ++i) {
        const UiMessage *msg = &m->messages[i];
        struct tm *local;
        char header[180], stamp[20] = "--:--";
        const char *status;
        if (peer && strcmp(peer->nickname, msg->peer)) continue;
        local = localtime(&msg->timestamp); if (local) strftime(stamp, sizeof(stamp), "%H:%M", local);
        status = msg->delivery == UI_ACCEPTED ? Ui_Text(m->language, "server queue", "очередь сервера")
            : msg->delivery == UI_FAILED ? Ui_Text(m->language, "failed", "ошибка")
            : msg->delivery == UI_RESTORED ? Ui_Text(m->language, "saved", "из журнала")
            : Ui_Text(m->language, "pending", "отправка");
        snprintf(header, sizeof(header), "%s  %s %s%s%s  %s", stamp, msg->outgoing ? "→" : "←",
            msg->outgoing ? m->nickname : msg->peer, msg->outgoing ? " → " : "",
            msg->outgoing ? msg->peer : "", msg->outgoing ? status : "");
        Wrapped(out, header, msg->outgoing ? C_ACCENT : C_GREEN);
        Wrapped(out, msg->text, C_BRIGHT); Wrapped(out, "", C_TEXT);
    }
}

static void Editor(const UiModel *m, CHAR_INFO *cells, int w, int h, int row, int rows, COORD *cursor)
{
    const char *p = m->input;
    int col = 0, caret = 0, window, available = (w - 7) * rows;
    while (*p) {
        unsigned c;
        if ((size_t)(p - m->input) == m->cursor) caret = col;
        c = Codepoint(&p); col += c == '\n' ? (w - 7) - col % (w - 7) : c > 0xffff ? 2 : 1;
    }
    if (m->cursor == strlen(m->input)) caret = col;
    window = caret >= available ? (caret / (w - 7) - rows + 1) * (w - 7) : 0;
    p = m->input; col = 0;
    while (*p) {
        unsigned c = Codepoint(&p);
        int slots = c > 0xffff ? 2 : 1, k;
        if (c == '\n') { col += (w - 7) - col % (w - 7); continue; }
        for (k = 0; k < slots; ++k) {
            unsigned glyph = c > 0xffff ? (k ? 0xdc00 + ((c - 0x10000) & 1023) : 0xd800 + ((c - 0x10000) >> 10)) : c;
            int pos = col++ - window;
            if (pos >= 0 && pos < available) Cell(cells, w, h, 4 + pos % (w - 7), row + pos / (w - 7), glyph, C_BRIGHT);
        }
    }
    *cursor = (COORD){(SHORT)(4 + (caret - window) % (w - 7)), (SHORT)(row + (caret - window) / (w - 7))};
}

static void File_Panel(const UiModel *m, CHAR_INFO *cells, int w, int h, COORD *cursor)
{
    int y, x, top = 9, bottom = h - 7, visible = bottom - top;
    unsigned first = m->browser_focus >= (unsigned)visible ? m->browser_focus - (unsigned)visible + 1 : 0, row;
    const char *title = m->file_dialog == 1 ? Ui_Text(m->language, "SELECT FILE TO SEND", "ВЫБОР ФАЙЛА ДЛЯ ОТПРАВКИ")
        : Ui_Text(m->language, "SAVE INCOMING FILE", "СОХРАНЕНИЕ ВХОДЯЩЕГО ФАЙЛА");
    x = Text(cells, w, h, 3, 5, w - 3, title, C_ACCENT);
    {
        const ProtocolParticipant *peer = Ui_Peer(m, m->file_dialog_peer);
        if (peer) Text(cells, w, h, x + 2, 5, w - 3, peer->nickname, C_GREEN);
    }
    if (m->file_dialog == 3) {
        Text(cells, w, h, 4, 7, w - 4, m->file.name, C_BRIGHT);
        Text(cells, w, h, 4, 9, w - 4, Ui_Text(m->language, "  downloads (next to client)", "  downloads (рядом с клиентом)"),
            m->destination_choice == 0 ? C_SELECT : C_TEXT);
        Text(cells, w, h, 4, 11, w - 4, Ui_Text(m->language, "  Choose another folder…", "  Выбрать другую папку…"),
            m->destination_choice == 1 ? C_SELECT : C_TEXT);
        Text(cells, w, h, 4, 13, w - 4, m->download_directory, C_MUTED);
        Text(cells, w, h, 4, h - 3, w - 4, Ui_Text(m->language, "↑↓ Choose · Enter Continue · Esc Back", "↑↓ Выбор · Enter Продолжить · Esc Назад"), C_ACCENT);
        *cursor = (COORD){4, 9}; return;
    }
    Text(cells, w, h, 4, 6, w - 4, *m->browser.directory ? m->browser.directory : Ui_Text(m->language, "COMPUTER · DRIVES", "КОМПЬЮТЕР · ДИСКИ"), C_BRIGHT);
    Rule(cells, w, h, 2, 7, w - 2);
    Text(cells, w, h, 4, 8, w - 4, Ui_Text(m->language, "NAME", "ИМЯ"), C_MUTED);
    if (w >= 65) Text(cells, w, h, w - 21, 8, w - 4, Ui_Text(m->language, "SIZE · BYTES", "РАЗМЕР · БАЙТЫ"), C_MUTED);
    if (m->browser_loading) Text(cells, w, h, 4, 10, w - 4, Ui_Text(m->language, "Reading folder…", "Чтение каталога…"), C_MUTED);
    else for (row = first, y = top; row <= m->browser.count && y < bottom; ++row, ++y) {
        const BrowserEntry *e = row ? &m->browser.entries[row - 1] : NULL;
        WORD color = row == m->browser_focus ? C_SELECT : e && e->directory ? C_ACCENT : C_TEXT;
        if (row == m->browser_focus) for (x = 3; x < w - 3; ++x) Cell(cells, w, h, x, y, ' ', color);
        Text(cells, w, h, 4, y, 7, !e || e->directory ? "▸" : "·", color);
        Text(cells, w, h, 7, y, w >= 65 ? w - 23 : w - 4, e ? e->name : "..", color);
        if (w >= 65 && e) {
            char size[32];
            if (e->directory) strcpy(size, Ui_Text(m->language, "<DIR>", "<КАТАЛОГ>"));
            else snprintf(size, sizeof(size), "%llu", (unsigned long long)e->size);
            Text(cells, w, h, w - 21, y, w - 4, size, color);
        }
    }
    Text(cells, w, h, 3, h - 6, w - 3, m->notice, C_ACCENT);
    Rule(cells, w, h, 2, h - 5, w - 2);
    Text(cells, w, h, 3, h - 4, w - 3, Ui_Text(m->language, "PATH · type a full path, then Enter", "ПУТЬ · полный путь, затем Enter"), C_MUTED);
    Editor(m, cells, w, h, h - 3, 1, cursor);
    Text(cells, w, h, 3, h - 1, w - 3, w < 78 ? (m->file_dialog == 1
        ? Ui_Text(m->language, "Enter Open/Send · F6 Drives · Esc", "Enter Выбор · F6 Диски · Esc Назад")
        : Ui_Text(m->language, "Enter Open · F7 Save · Esc Back", "Enter Вход · F7 Сюда · Esc Назад")) : m->file_dialog == 1
        ? Ui_Text(m->language, "Enter Open/Send · Backspace Up · F5 Refresh · F6 Drives · Esc Back", "Enter Открыть/Отправить · Backspace Вверх · F5 Обновить · F6 Диски · Esc Назад")
        : Ui_Text(m->language, "Enter Open · F7 Save here · F6 Drives · Esc Back", "Enter Открыть · F7 Сохранить здесь · F6 Диски · Esc Назад"), C_MUTED);
}

void Console_Layout(const UiModel *m, CHAR_INFO *cells, int w, int h, COORD *cursor)
{
    int i, side = w >= 78 ? 26 : 0, left = side ? side + 2 : 3;
    int top = 7, bottom, base, hint_rows, x, y;
    unsigned hints = Ui_Completions(m);
    char line[256];
    const ProtocolParticipant *peer = Ui_Peer(m, m->selected);
    Lines lines = {NULL, w, h, left, w - 3, top, 0, 0, 0};
    for (i = 0; i < w * h; ++i) { cells[i].Char.UnicodeChar = L' '; cells[i].Attributes = C_TEXT; }
    *cursor = (COORD){0, 0};
    if (w < 40 || h < 18) {
        Text(cells, w, h, 1, 1, w - 1, Ui_Text(m->language, "Resize window: at least 40 x 18", "Увеличьте окно: минимум 40 x 18"), C_ACCENT); return;
    }
    hint_rows = (int)hints; if (hint_rows > 5) hint_rows = 5;
    if (hint_rows > h - 16) hint_rows = h - 16;
    base = h - hint_rows; bottom = base - 8; lines.bottom = bottom;
    Text(cells, w, h, 2, 1, w - 2, "╭›╮  " TERMTALK_NAME, C_GREEN);
    if (w >= 78) Text(cells, w, h, 2, 2, w - 2, Ui_Text(m->language, "╰─╯  PRIVATE MESSENGER", "╰─╯  ЛИЧНЫЕ СООБЩЕНИЯ"), C_MUTED);
    snprintf(line, sizeof(line), "%s  ·  %s", m->endpoint, m->online > 0 ? Ui_Text(m->language, "● ONLINE", "● В СЕТИ")
        : m->online == -1 ? Ui_Text(m->language, "○ LOGIN", "○ ВХОД")
        : m->online == -2 ? Ui_Text(m->language, "○ CONNECTING", "○ ПОДКЛЮЧЕНИЕ")
        : m->online == -3 ? Ui_Text(m->language, "○ NOT CONNECTED", "○ НЕТ ПОДКЛЮЧЕНИЯ")
        : Ui_Text(m->language, "○ REGISTERING", "○ РЕГИСТРАЦИЯ"));
    Text(cells, w, h, w < 78 ? 2 : w > 95 ? w - 49 : 27, w < 78 ? 2 : 1, w - 2, line, m->online > 0 ? C_GREEN : C_MUTED);
    snprintf(line, sizeof(line), "%s  ·  %s  ·  %s", m->nickname, m->language == UI_EN ? "EN" : "RU",
        m->saving ? Ui_Text(m->language, "history on", "история вкл") : Ui_Text(m->language, "history off", "история выкл"));
    Text(cells, w, h, w < 78 ? 2 : w > 95 ? w - 49 : 27, w < 78 ? 3 : 2, w - 2, line, C_MUTED);
    if (!m->server_menu && m->online > 0) {
        int quality = Ui_Quality(m->latency, m->latency_state == 1);
        const char *bars = m->latency_state == 0 ? "···" : quality == 3 ? "█▀▔" : quality == 2 ? "▀▔" : "▔";
        char ping[32], capacity[16];
        if (m->capacity) snprintf(capacity, sizeof(capacity), "%u", m->capacity); else strcpy(capacity, "?");
        if (m->latency_state == 1) snprintf(ping, sizeof(ping), "%u ms", m->latency);
        else snprintf(ping, sizeof(ping), "%s", Ui_Text(m->language, "pending", "ожидание"));
        snprintf(line, sizeof(line), "%s  %u/%s  ·  %s", bars, m->count, capacity, ping);
        Text(cells, w, h, w >= 78 ? (w > 95 ? w - 49 : 27) : 2, 3, w - 2, line,
            !m->latency_state ? C_MUTED : quality == 3 ? C_GREEN : quality == 2 ? C_YELLOW : C_RED);
    }
    Rule(cells, w, h, 2, 4, w - 2);
    if (m->file_dialog) { File_Panel(m, cells, w, h, cursor); return; }
    if (m->server_menu) {
        int visible = bottom - top, first = (int)m->server_focus >= visible ? (int)m->server_focus - visible + 1 : 0;
        Text(cells, w, h, 3, 5, w - 3, Ui_Text(m->language, "SERVERS · choose with arrows or enter IPv4:port", "СЕРВЕРЫ · выберите стрелками или введите IPv4:порт"), C_BRIGHT);
        for (i = first; i < (int)m->server_count && top + i - first < bottom; ++i) {
            WORD color = m->server_focus == (unsigned)i ? C_SELECT : C_TEXT;
            snprintf(line, sizeof(line), "%s %s:%u%s", m->server_focus == (unsigned)i ? "›" : " ", m->servers[i].ip,
                m->servers[i].port, i == 0 ? Ui_Text(m->language, "  default", "  стандартный") : "");
            if (color == C_SELECT) for (x = 2; x < w - 2; ++x) Cell(cells, w, h, x, top + i - first, ' ', color);
            Text(cells, w, h, 3, top + i - first, w - 3, line, color);
        }
        if (m->probe_state == 2) {
            int quality = Ui_Quality(m->probe_latency, m->probe_latency_valid);
            snprintf(line, sizeof(line), "%s  %s · %u/%u · %s %u ms", quality == 3 ? "█▀▔" : quality == 2 ? "▀▔" : "▔",
                Ui_Text(m->language, "online", "онлайн"), m->probe_users, m->probe_capacity,
                Ui_Text(m->language, "ping", "пинг"), m->probe_latency);
            if (!m->probe_latency_valid) snprintf(line, sizeof(line), "%s · %u/%u · %s", Ui_Text(m->language, "online", "онлайн"),
                m->probe_users, m->probe_capacity, Ui_Text(m->language, "ping pending", "ожидается пинг"));
            Text(cells, w, h, 3, h - 8, w - 3, line, quality == 3 ? C_GREEN : quality == 2 ? C_YELLOW : C_RED);
        } else Text(cells, w, h, 3, h - 8, w - 3, m->probe_state == 3 ? Ui_Text(m->language, "▔ No response / unavailable", "▔ Нет ответа / недоступен")
            : Ui_Text(m->language, "Checking server… F5 to refresh", "Проверка сервера… F5: обновить"), m->probe_state == 3 ? C_RED : C_MUTED);
    } else if (m->online == -1) {
        Text(cells, w, h, 4, 7, w - 4, Ui_Text(m->language, "Welcome. Choose your nickname", "Добро пожаловать. Введите никнейм"), C_BRIGHT);
        Text(cells, w, h, 4, 9, w - 4, Ui_Text(m->language, "1..32 UTF-8 bytes · Enter save · Esc cancel · F2 EN/RU", "1..32 байта UTF-8 · Enter сохранить · Esc отмена · F2 EN/RU"), C_MUTED);
    } else {
        if (side || m->contacts) {
            int end = side ? side - 1 : w - 3;
            int visible = (bottom - top + 1) / 2;
            int first = m->contacts && (int)m->focus >= visible ? (int)m->focus - visible + 1 : 0;
            snprintf(line, sizeof(line), "%s  %u", Ui_Text(m->language, "PARTICIPANTS", "УЧАСТНИКИ"), m->count);
            Text(cells, w, h, 3, 5, end, line, m->contacts ? C_ACCENT : C_MUTED);
            for (i = first; i < (int)m->count && top + (i - first) * 2 < bottom; ++i) {
                WORD color = m->contacts && m->focus == (unsigned)i ? C_SELECT : m->users[i].id == m->selected ? C_ACCENT : C_TEXT;
                snprintf(line, sizeof(line), "%s %lu %s%s", m->users[i].id == m->own_id ? "○" : "●",
                    (unsigned long)m->users[i].id, m->users[i].nickname, m->unread[i] ? " *" : "");
                if (color == C_SELECT) for (x = 2; x < end; ++x) Cell(cells, w, h, x, top + (i - first) * 2, ' ', color);
                Text(cells, w, h, 3, top + (i - first) * 2, end, line, color);
            }
        }
        if (side) for (y = 5; y < bottom; ++y) Cell(cells, w, h, side, y, 0x2502, C_MUTED);
        if (side || !m->contacts) {
            snprintf(line, sizeof(line), "%s  %s%s", Ui_Text(m->language, "PRIVATE CHAT", "ЛИЧНЫЙ ЧАТ"),
                peer ? peer->nickname : Ui_Text(m->language, "Select a participant", "Выберите участника"), m->scroll ? " ↑" : "");
            Text(cells, w, h, left, 5, w - 3, line, C_BRIGHT);
            if (!m->message_count) {
                Text(cells, w, h, left, top + 1, w - 3, Ui_Text(m->language, "Your conversation starts here.", "Здесь начинается ваш разговор."), C_MUTED);
                Text(cells, w, h, left, top + 3, w - 3, Ui_Text(m->language, "Tab → arrows → Enter to choose someone", "Tab → стрелки → Enter: выбрать собеседника"), C_MUTED);
            }
            Conversation(&lines, m); lines.start = lines.line - (bottom - top) - m->scroll;
            if (lines.start < 0) lines.start = 0;
            lines.line = 0; lines.cells = cells; Conversation(&lines, m);
        }
    }
    if (!m->server_menu && m->file.status != FILE_IDLE) {
        const char *state = m->file.status == FILE_COMPLETED ? Ui_Text(m->language, "complete", "завершено")
            : m->file.status == FILE_FAILED ? Ui_Text(m->language, "failed", "ошибка")
            : m->file.status == FILE_REFUSED ? Ui_Text(m->language, "declined", "отказ")
            : m->file.status == FILE_CANCELLED ? Ui_Text(m->language, "cancelled", "отменено")
            : m->file.status == FILE_OFFERED ? Ui_Text(m->language, "offer", "предложение")
            : Ui_Text(m->language, "transfer", "передача");
        snprintf(line, sizeof(line), "%s #%lu %llu/%llu · %s · ", Ui_Text(m->language, "FILE", "ФАЙЛ"),
            (unsigned long)m->file.id, (unsigned long long)m->file.bytes, (unsigned long long)m->file.total, state);
        x = Text(cells, w, h, 3, base - 8, w - 3, line, C_GREEN);
        Text(cells, w, h, x, base - 8, w - 3, m->file.name, C_TEXT);
    }
    Text(cells, w, h, 3, base - 7, w - 3, m->notice, C_ACCENT);
    Rule(cells, w, h, 2, base - 6, w - 2);
    Text(cells, w, h, 3, base - 5, w - 3, m->server_menu ? Ui_Text(m->language, "IPv4:PORT  ·  empty input selects highlighted server", "IPv4:ПОРТ  ·  пустая строка: выделенный сервер")
        : m->online == -1 ? Ui_Text(m->language, "NICKNAME", "НИКНЕЙМ")
        : Ui_Text(m->language, "MESSAGE  ·  Enter send / Shift+Enter new line", "СООБЩЕНИЕ  ·  Enter отправить / Shift+Enter новая строка"), C_MUTED);
    Editor(m, cells, w, h, base - 4, 2, cursor);
    Text(cells, w, h, 2, base - 4, 4, "›", C_ACCENT);
    if (hint_rows) {
        unsigned selected = m->completion_focus < hints ? m->completion_focus : 0;
        unsigned first = selected >= (unsigned)hint_rows ? selected - (unsigned)hint_rows + 1 : 0, row;
        for (row = 0; row < (unsigned)hint_rows; ++row) {
            const CommandHint *hint = Command_Match(m->input, first + row);
            WORD color = first + row == selected ? C_SELECT : C_MUTED;
            y = base - 2 + (int)row;
            if (first + row == selected) for (x = 3; x < w - 3; ++x) Cell(cells, w, h, x, y, ' ', color);
            x = Text(cells, w, h, 4, y, w - 4, hint->name, color);
            if (w >= 65) Text(cells, w, h, x + 2, y, w - 4, Ui_Text(m->language, hint->english, hint->russian), color);
        }
    }
    Rule(cells, w, h, 2, h - 2, w - 2);
    Text(cells, w, h, 3, h - 1, w - 2, m->server_menu && m->online == -3 ? Ui_Text(m->language,
        "F7 Nickname  F2 EN/RU  F5 Status  Enter Connect  Ctrl+Q Quit",
        "F7 Никнейм  F2 EN/RU  F5 Статус  Enter Вход  Ctrl+Q Выход") : Ui_Text(m->language,
        "F1 Help  F2 EN/RU  F3 Servers  F4 Disconnect  F6 File  Tab Complete/Contacts  Ctrl+Q Quit",
        "F1 Справка  F2 EN/RU  F3 Серверы  F4 Выход с сервера  F6 Файл  Tab Подсказка/Участники"), C_MUTED);
    if (m->help) {
        Lines help = {cells, w, h, 4, w - 4, 6, h - 3, 0, m->scroll};
        for (y = 5; y < h - 2; ++y) for (x = 2; x < w - 2; ++x) Cell(cells, w, h, x, y, ' ', C_TEXT);
        Wrapped(&help, Ui_Help(m->language), C_BRIGHT);
    }
}

int Console_Open(ConsoleUi *ui)
{
    DWORD mode;
    memset(ui, 0, sizeof(*ui)); ui->screen = INVALID_HANDLE_VALUE;
    ui->input = GetStdHandle(STD_INPUT_HANDLE); ui->original = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(ui->input, &ui->input_mode) || !GetConsoleMode(ui->original, &mode)) return 1;
    ui->screen = CreateConsoleScreenBuffer(GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CONSOLE_TEXTMODE_BUFFER, NULL);
    if (ui->screen == INVALID_HANDLE_VALUE) return 0;
    Palette(ui->screen);
    ui->interactive = 1;
    if (!SetConsoleMode(ui->input, (ui->input_mode | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS)
            & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE))
        || !SetConsoleActiveScreenBuffer(ui->screen)) { Console_Close(ui); return 0; }
    return 1;
}

void Console_Close(ConsoleUi *ui)
{
    if (ui->interactive) { SetConsoleActiveScreenBuffer(ui->original); SetConsoleMode(ui->input, ui->input_mode); }
    if (ui->screen != INVALID_HANDLE_VALUE && ui->screen != NULL) CloseHandle(ui->screen);
    free(ui->previous); free(ui->cells); memset(ui, 0, sizeof(*ui)); ui->screen = INVALID_HANDLE_VALUE;
}

int Console_Draw(ConsoleUi *ui, const UiModel *model)
{
    CONSOLE_SCREEN_BUFFER_INFO info;
    CONSOLE_CURSOR_INFO ci = {25, TRUE};
    COORD cursor;
    int w, h, y;
    if (!ui->interactive) return 1;
    if (!GetConsoleScreenBufferInfo(ui->screen, &info)) return 0;
    w = info.srWindow.Right - info.srWindow.Left + 1; h = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (w > 320) w = 320;
    if (h > 160) h = 160;
    if (w != ui->width || h != ui->height) {
        CHAR_INFO *cells = calloc((size_t)w * h, sizeof(*cells)), *old = calloc((size_t)w * h, sizeof(*old));
        if (!cells || !old) { free(cells); free(old); return 0; }
        free(ui->cells); free(ui->previous); ui->cells = cells; ui->previous = old; ui->width = w; ui->height = h;
    }
    Console_Layout(model, ui->cells, w, h, &cursor);
    for (y = 0; y < h; ++y) {
        int start = 0, end = w - 1;
        SMALL_RECT region;
        while (start < w && !memcmp(&ui->cells[y * w + start], &ui->previous[y * w + start], sizeof(CHAR_INFO))) ++start;
        if (start == w) continue;
        while (end > start && !memcmp(&ui->cells[y * w + end], &ui->previous[y * w + end], sizeof(CHAR_INFO))) --end;
        region = (SMALL_RECT){(SHORT)(info.srWindow.Left + start), (SHORT)(info.srWindow.Top + y),
            (SHORT)(info.srWindow.Left + end), (SHORT)(info.srWindow.Top + y)};
        if (!WriteConsoleOutputW(ui->screen, ui->cells, (COORD){(SHORT)w, (SHORT)h}, (COORD){(SHORT)start, (SHORT)y}, &region)) return 0;
    }
    memcpy(ui->previous, ui->cells, (size_t)w * h * sizeof(CHAR_INFO));
    cursor.X += info.srWindow.Left; cursor.Y += info.srWindow.Top;
    ci.bVisible = !model->help && !model->contacts && model->file_dialog != 3;
    return SetConsoleCursorInfo(ui->screen, &ci) && SetConsoleCursorPosition(ui->screen, cursor);
}

UiAction Console_Key(ConsoleUi *ui, UiModel *m, const KEY_EVENT_RECORD *key)
{
    int ctrl = (key->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0
        && !(key->dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED));
    WORD code = key->wVirtualKeyCode;
    wchar_t c = key->uChar.UnicodeChar;
    if (!key->bKeyDown) return UI_IDLE;
    if (ctrl && (code == 'Q' || code == 'C')) return UI_QUIT;
    if (code == VK_F2) {
        m->language = m->language == UI_EN ? UI_RU : UI_EN;
        snprintf(m->notice, sizeof(m->notice), "%s", Ui_Text(m->language, "Language: English.", "Язык: Русский."));
        return UI_CHANGED;
    }
    if (m->file_dialog) {
        if (code == VK_ESCAPE) return UI_FILE_CLOSE;
        if (m->file_dialog == 3) {
            if (code == VK_UP || code == VK_DOWN || code == VK_TAB) { m->destination_choice = !m->destination_choice; return UI_CHANGED; }
            if (code == VK_RETURN) return m->destination_choice ? UI_FILE_BROWSE : UI_FILE_PICK;
            return UI_IDLE;
        }
        if (code == VK_F5) return UI_FILE_REFRESH;
        if (code == VK_F6) return UI_FILE_DRIVES;
        if (code == VK_F7 && m->file_dialog == 2) return UI_FILE_PICK;
        if (code == VK_RETURN) return UI_FILE_ENTER;
        if (code == VK_BACK && !*m->input) return UI_FILE_PARENT;
        if (ctrl && code == 'L') { strcpy(m->input, m->browser.directory); m->cursor = strlen(m->input); return UI_CHANGED; }
        if (code == VK_TAB) return UI_IDLE;
        if (code == VK_UP || code == VK_DOWN || code == VK_PRIOR || code == VK_NEXT) {
            unsigned step = code == VK_PRIOR || code == VK_NEXT ? 10 : 1;
            if (code == VK_UP || code == VK_PRIOR) m->browser_focus = m->browser_focus > step ? m->browser_focus - step : 0;
            else { m->browser_focus += step; if (m->browser_focus > m->browser.count) m->browser_focus = m->browser.count; }
            return UI_CHANGED;
        }
        if (code >= VK_F1 && code <= VK_F12) return UI_IDLE;
    }
    if (code == VK_F6 && m->online > 0 && !m->server_menu && !m->file_dialog) return UI_FILE_OPEN;
    {
        unsigned count = Ui_Completions(m);
        if (count && (code == VK_UP || code == VK_DOWN)) {
            if (m->completion_focus >= count) m->completion_focus = 0;
            m->completion_focus = code == VK_DOWN ? (m->completion_focus + 1) % count : (m->completion_focus + count - 1) % count;
            return UI_CHANGED;
        }
        if (count && code == VK_TAB) {
            const CommandHint *hint = Command_Match(m->input, m->completion_focus < count ? m->completion_focus : 0);
            snprintf(m->input, sizeof(m->input), "%s%s", hint->name, *hint->arguments ? " " : "");
            m->cursor = strlen(m->input); m->completion_hidden = 1; return UI_CHANGED;
        }
        if (count && code == VK_ESCAPE) { m->completion_hidden = 1; return UI_CHANGED; }
    }
    if (code == VK_F1) { m->help = !m->help; m->scroll = 0; return UI_CHANGED; }
    if (code == VK_F4 && m->online > 0) return UI_DISCONNECT;
    if (code == VK_F3 && m->online != -1) { Ui_ServerMenu(m, !m->server_menu); return UI_CHANGED; }
    if (code == VK_ESCAPE && m->online == -1 && !m->help && *m->nickname) return UI_NICKNAME_CANCEL;
    if (code == VK_ESCAPE) { if (!m->help && m->server_menu) Ui_ServerMenu(m, 0); m->help = m->contacts = m->scroll = 0; return UI_CHANGED; }
    if (code == VK_PRIOR) { if (m->scroll < 1048576) m->scroll += 5; return UI_CHANGED; }
    if (code == VK_NEXT) { m->scroll -= 5; if (m->scroll < 0) m->scroll = 0; return UI_CHANGED; }
    if (m->help) return UI_IDLE;
    if (m->server_menu) {
        if (code == VK_F7 && m->online == -3) return UI_NICKNAME;
        if (code == VK_F5) return UI_SERVER_CHECK;
        if (code == VK_UP) { if (m->server_focus) --m->server_focus; m->input[0] = 0; m->cursor = 0; return UI_CHANGED; }
        if (code == VK_DOWN) { if (m->server_focus + 1 < m->server_count) ++m->server_focus; m->input[0] = 0; m->cursor = 0; return UI_CHANGED; }
        if (code == VK_RETURN) return UI_SERVER_CONNECT;
        if (code == VK_TAB) return UI_IDLE;
    }
    if (ctrl && code == 'R') return UI_REFRESH;
    if (ctrl && code == 'U') { m->input[0] = 0; m->cursor = 0; m->command_position = 0; m->completion_hidden = 0; m->completion_focus = 0; return UI_CHANGED; }
    if (code == VK_TAB && m->online >= 0) { m->contacts = !m->contacts; return UI_CHANGED; }
    if (m->contacts) {
        if (code == VK_UP && m->focus) --m->focus;
        if (code == VK_DOWN && m->focus + 1 < m->count) ++m->focus;
        if (code == VK_RETURN && m->focus < m->count && m->users[m->focus].id != m->own_id) {
            m->selected = m->users[m->focus].id; m->unread[m->focus] = 0; m->contacts = m->scroll = 0;
        }
        return UI_CHANGED;
    }
    if (!m->file_dialog && m->online > 0 && (code == VK_UP || code == VK_DOWN))
        return Ui_RecallCommand(m, code == VK_UP ? -1 : 1) ? UI_CHANGED : UI_IDLE;
    if (code == VK_LEFT) Ui_Edit(m, -1);
    else if (code == VK_RIGHT) Ui_Edit(m, 1);
    else if (code == VK_BACK) Ui_Edit(m, -2);
    else if (code == VK_DELETE) Ui_Edit(m, 2);
    else if (code == VK_HOME) m->cursor = 0;
    else if (code == VK_END) m->cursor = strlen(m->input);
    else if (code == VK_RETURN) {
        if (key->dwControlKeyState & SHIFT_PRESSED) Ui_Insert(m, "\n");
        else return UI_SUBMIT;
    } else if (c >= 32 && c != 127) {
        wchar_t wide[3] = {c, 0, 0};
        char utf8[9];
        if (c >= 0xd800 && c <= 0xdbff) { ui->high_surrogate = c; return UI_IDLE; }
        if (c >= 0xdc00 && c <= 0xdfff) { if (!ui->high_surrogate) return UI_IDLE; wide[0] = ui->high_surrogate; wide[1] = c; }
        ui->high_surrogate = 0;
        if (WideCharToMultiByte(CP_UTF8, 0x80, wide, -1, utf8, sizeof(utf8), NULL, NULL)) {
            unsigned repeat;
            for (repeat = 0; repeat < key->wRepeatCount; ++repeat) if (!Ui_Insert(m, utf8)) {
                snprintf(m->notice, sizeof(m->notice), "%s", Ui_Text(m->language, "Input limit reached", "Достигнут предел ввода")); break;
            }
        }
    } else return UI_IDLE;
    m->completion_hidden = 0; m->completion_focus = 0;
    return UI_CHANGED;
}

UiAction Console_Poll(ConsoleUi *ui, UiModel *m)
{
    if (ui->interactive) {
        INPUT_RECORD record;
        DWORD count;
        if (!GetNumberOfConsoleInputEvents(ui->input, &count)) return UI_INPUT_ERROR;
        if (!count) return UI_IDLE;
        if (!ReadConsoleInputW(ui->input, &record, 1, &count)) return UI_INPUT_ERROR;
        if (record.EventType == KEY_EVENT) return Console_Key(ui, m, &record.Event.KeyEvent);
        return record.EventType == WINDOW_BUFFER_SIZE_EVENT ? UI_CHANGED : UI_IDLE;
    }
    if (ui->eof) return UI_EOF;
    {
        unsigned budget = 512;
        while (budget--) {
        DWORD available = 0, read;
        char c = 0;
        if (GetFileType(ui->input) == FILE_TYPE_PIPE && !PeekNamedPipe(ui->input, NULL, 0, NULL, &available, NULL)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) return UI_INPUT_ERROR;
            ui->eof = 1;
        } else if (GetFileType(ui->input) == FILE_TYPE_PIPE && !available) return UI_IDLE;
        if (!ui->eof && (!ReadFile(ui->input, &c, 1, &read, NULL) || !read)) ui->eof = 1;
        if (ui->eof || c == '\n') {
            if (ui->invalid) return UI_INPUT_ERROR;
            ui->pipe_line[ui->pipe_used] = 0;
            if (!ui->pipe_used) return ui->eof ? UI_EOF : UI_IDLE;
            strcpy(m->input, ui->pipe_line); m->cursor = ui->pipe_used; ui->pipe_used = 0; return UI_SUBMIT;
        }
        if (c != '\r') {
            if (!c || ui->pipe_used + 1 >= sizeof(ui->pipe_line)) ui->invalid = 1;
            else ui->pipe_line[ui->pipe_used++] = c;
        }
        }
    }
    return UI_IDLE;
}
void Console_Log(const ConsoleUi *ui, const char *text)
{ if (!ui->interactive) { fputs(text, stdout); fflush(stdout); } }

void Console_Report(const char *text)
{
    HANDLE output = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode, written;
    int count = MultiByteToWideChar(CP_UTF8, 0x8, text, -1, NULL, 0);
    if (GetConsoleMode(output, &mode) && count > 0) {
        wchar_t *wide = malloc((size_t)count * sizeof(*wide));
        if (wide) {
            if (MultiByteToWideChar(CP_UTF8, 0x8, text, -1, wide, count))
                WriteConsoleW(output, wide, (DWORD)count - 1, &written, NULL);
            free(wide); return;
        }
    }
    fputs(text, stderr); fflush(stderr);
}
