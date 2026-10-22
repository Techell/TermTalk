#ifndef WINSOCK_CONSOLE_UI_H
#define WINSOCK_CONSOLE_UI_H
/** @file console_ui.h @brief Заменяемый Windows-адаптер экрана и Unicode-ввода, без сетевых операций. */
#include "network.h"
#include "ui_model.h"
/** @brief Результат опроса; редактирование/навигация уже применены к модели. */
typedef enum { UI_IDLE, UI_CHANGED, UI_SUBMIT, UI_QUIT, UI_REFRESH, UI_EOF, UI_INPUT_ERROR,
    UI_SERVER_CONNECT, UI_SERVER_CHECK, UI_DISCONNECT, UI_FILE_OPEN, UI_FILE_ENTER,
    UI_FILE_PARENT, UI_FILE_PICK, UI_FILE_CLOSE, UI_FILE_DRIVES, UI_FILE_REFRESH,
    UI_FILE_BROWSE,
    UI_NICKNAME, /**< Открыть форму имени из отключённого меню; сеть не меняется. */
    UI_NICKNAME_CANCEL /**< Отменить форму с сохранением прежнего имени и адреса. */
} UiAction;
/** @brief Адаптер владеет экраном/буферами; все методы вызывает основной поток. */
typedef struct {
    HANDLE input; /**< Заимствованный stdin. */
    HANDLE original; /**< Заимствованный исходный stdout/экран. */
    HANDLE screen; /**< Собственный экран либо INVALID_HANDLE_VALUE. */
    DWORD input_mode; /**< Сохранённый режим исходного ввода. */
    int interactive; /**< Используется полноценный экран, а не текстовые потоки. */
    int width; /**< Ширина кадрового буфера, максимум 320. */
    int height; /**< Высота кадрового буфера, максимум 160. */
    int eof; /**< Достигнут EOF перенаправленного stdin. */
    int invalid; /**< Неверная либо слишком длинная перенаправленная строка. */
    CHAR_INFO *previous; /**< Собственная копия последнего успешно записанного кадра. */
    CHAR_INFO *cells; /**< Собственный составляемый кадр width*height. */
    size_t pipe_used; /**< Действующие байты pipe_line. */
    char pipe_line[UI_INPUT_MAX]; /**< Незавершённая строка перенаправленного stdin. */
    wchar_t high_surrogate; /**< Первая половина пары UTF-16 либо 0. */
} ConsoleUi;
/** @brief Открыть отдельный экран либо текстовый режим при перенаправлении.
 * @param ui Выходной объект вызывающего, закрываемый Console_Close.
 * @return 1 успех, 0 ошибка (ресурсы восстановлены). */
int Console_Open(ConsoleUi *ui);
/** @brief Восстановить исходный экран и режим ввода, освободить память.
 * @param ui Инициализированный объект; повторное закрытие допустимо. Сокеты не затрагиваются. */
void Console_Close(ConsoleUi *ui);
/** @brief Составить кадр без обращения к консоли, для проверок и замены адаптера.
 * @param model Неизменяемая модель. @param cells Буфер вызывающего width*height CHAR_INFO.
 * @param width Ширина 1..320. @param height Высота 1..160. @param cursor Выходная позиция курсора.
 * @pre Указатели ненулевые; функция не удерживает их и не меняет модель. */
void Console_Layout(const UiModel *model, CHAR_INFO *cells, int width, int height, COORD *cursor);
/** @brief Перезаписать только изменившиеся отрезки строк с учётом размера окна.
 * @param ui Открытый адаптер. @param model Модель основного потока.
 * @return 1 успех/текстовый режим, 0 ошибка WinAPI/ресурсов. */
int Console_Draw(ConsoleUi *ui, const UiModel *model);
/** @brief Опросить одно событие Unicode/оконного/перенаправленного ввода без ожидания строки.
 * @param ui Адаптер. @param model Изменяемая модель основного потока.
 * @return Действие; UI_SUBMIT оставляет строку в model->input до обработки. */
UiAction Console_Poll(ConsoleUi *ui, UiModel *model);
/** @brief Применить синтетический либо полученный WinAPI KEY_EVENT_RECORD.
 * @param ui Адаптер, хранит незавершённый surrogate. @param model Модель. @param key Заимствованное событие.
 * @return Действие; отпускание игнорируется. Владение не меняется. */
UiAction Console_Key(ConsoleUi *ui, UiModel *model, const KEY_EVENT_RECORD *key);
/** @brief Вывести доверенный UTF-8 только в перенаправленный stdout.
 * @param ui Адаптер. @param text Строка с NUL; при интерактивном режиме вывод подавлен. */
void Console_Log(const ConsoleUi *ui, const char *text);
/** @brief Показать итоговую диагностику в исходном stderr после восстановления экрана.
 * @param text Доверенная UTF-8 строка с NUL; функция не удерживает её.
 * @note Только основной поток. Для настоящей консоли использует Unicode WriteConsoleW,
 *       при перенаправлении пишет исходный UTF-8. Ресурсы стандартного потока заимствованы. */
void Console_Report(const char *text);
#endif
