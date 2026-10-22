#ifndef WINSOCK_CHAT_H
#define WINSOCK_CHAT_H
/** @file chat.h @brief Контроллер приложения: связывает транспорт, модель, UI и локальную историю. */
#include "ui_model.h"
#include <wchar.h>
/** @brief Настройки сеанса, строки заимствуются до возврата Chat_Run. */
typedef struct {
    UiLanguage language; /**< Начальный язык интерфейса; меняется внутри сеанса. */
    int save_history; /**< Читать/сохранять историю по умолчанию; 0 отключает оба действия. */
    const wchar_t *history_directory; /**< Заимствованный каталог UTF-16 либо NULL для LOCALAPPDATA. */
    int choose_server; /**< Открыть меню адресов при интерактивном запуске без позиционного IP. */
} ChatOptions;
/** @brief Выполнить приложение с меню серверов, входом и единственным владельцем экрана.
 * @param address IPv4 с NUL. @param port Ненулевой порт.
 * @param nickname Проверенный UTF-8 ник 1..32 байта либо NULL для формы входа (Guest при перенаправлении).
 * @param options Настройки; ненулевой указатель, живущий до возврата.
 * @return 0 штатное завершение, -1 ошибка ввода/ресурсов/сети.
 * @pre Один активный сеанс консоли в процессе.
 * @note Функция создаёт и закрывает Client, Winsock и адаптеры; сетевой поток
 *       прекращается до закрытия сокета. Смена сервера создаёт новый сеанс;
 *       никнейм/язык сохраняются, ID и история привязаны к endpoint.
 *       Параметры не освобождаются и не удерживаются после возврата. */
int Chat_Run(const char *address, unsigned short port, const char *nickname, const ChatOptions *options);
#endif
