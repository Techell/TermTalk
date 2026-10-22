#ifndef WINSOCK_SERVER_BOOK_H
#define WINSOCK_SERVER_BOOK_H
/** @file server_book.h @brief Проверяемые адреса IPv4 и отдельная локальная история серверов. */
#include <stdint.h>
#include <wchar.h>
/** Максимум сохранённых адресов, включая стандартный. */
#define SERVER_BOOK_MAX 16
/** @brief Адрес владеет строкой; статус не сохраняется, чтобы не выдавать старое за текущее. */
typedef struct {
    char ip[16]; /**< Канонический IPv4 с NUL. */
    uint16_t port; /**< Порт хоста 1..65535. */
} ServerAddress;
/** @brief Книга одного владельца, стандартный адрес всегда в позиции 0. */
typedef struct {
    ServerAddress entries[SERVER_BOOK_MAX]; /**< Собственные копии адресов. */
    unsigned count; /**< Число записей 1..16. */
    wchar_t directory[1024]; /**< Собственный каталог UTF-16. */
    wchar_t path[1200]; /**< Собственный путь servers.wss. */
} ServerBook;
/** @brief Разобрать IPv4[:порт] либо IPv4 пробел порт, пусто/default — 127.0.0.1:5050.
 * @param text Строка ASCII с NUL. @param address Выходная копия только при успехе.
 * @return 1 корректно, 0 ошибка; не обращается к сети, владение не меняется. */
int ServerAddress_Parse(const char *text, ServerAddress *address);
/** @brief Инициализировать стандартный адрес и пути без создания файлов.
 * @param book Выходной объект. @param directory Заимствованный каталог UTF-16 либо NULL для LOCALAPPDATA/WinSock.
 * @return 1 путь готов, 0 ошибка; стандартный адрес доступен даже при ошибке пути. */
int ServerBook_Init(ServerBook *book, const wchar_t *directory);
/** @brief Загрузить ограниченный WSS1 целиком без частичных изменений.
 * @param book Инициализированный объект владельца.
 * @return 1 успех/файл отсутствует, 0 повреждение или ошибка. Не создаёт файлы. */
int ServerBook_Load(ServerBook *book);
/** @brief Добавить адрес в начало недавних, убрать дубликат/старейший сверх лимита.
 * @param book Объект владельца. @param address Проверенный адрес, копируется.
 * @note Стандартная запись 0 остаётся закреплена, диск не изменяется. */
void ServerBook_Add(ServerBook *book, const ServerAddress *address);
/** @brief Атомарно сохранить WSS1 с временным файлом того же каталога.
 * @param book Неизменяемая книга с подготовленным путём.
 * @return 1 успех, 0 ошибка, старый файл до успешной замены сохраняется. */
int ServerBook_Save(const ServerBook *book);
#endif
