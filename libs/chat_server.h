#ifndef WINSOCK_CHAT_SERVER_H
#define WINSOCK_CHAT_SERVER_H
/** @file chat_server.h
 * @brief Регистрация, личные сообщения, статус и маршруты файлов WSCK v2.
 */
#include "server_loop.h"
#include "protocol.h"

/** Непрозрачное приложение, единолично владеющее транспортным ServerLoop. */
typedef struct ChatServer ChatServer;

/** @brief Создать сервер с 10 участниками и пределом одного файла 32 ГиБ.
 * @param[in] port Порт хоста, 0 — автоматический выбор.
 * @pre Winsock запущен; все вызовы делает один владелец.
 * @return Владеющий указатель либо NULL с причиной WSAGetLastError.
 * @note При ошибке очищает промежуточные ресурсы; при успехе нужен Destroy.
 */
ChatServer *ChatServer_Create(u_short port);
/** @brief Создать сервер с лимитами участников и одного файла.
 * @param port Порт, 0 для автоматического. @param max_users Число участников 1..32.
 * @param max_file_bytes Предел одного файла 0..1 ТиБ; 0 запрещает передачи.
 * @return Владеющий объект либо NULL с Winsock-ошибкой; неверные лимиты — WSAEINVAL.
 * @pre Winsock запущен; создание, Poll и Destroy выполняет один поток.
 * @note Один дополнительный транспортный слот обслуживает STATUS при полном сервере.
 *       Память ограничена, файлы на сервере не сохраняются; освобождает Destroy. */
ChatServer *ChatServer_CreateConfigured(u_short port, unsigned max_users, uint64_t max_file_bytes);

/** @brief Закрыть все сеансы и транспорт, освободить приложение.
 * @param[in,out] server Владеющий указатель либо NULL; после вызова недействителен.
 * @pre Нет параллельного Poll. @note Не ждёт доставки TX; Winsock остаётся приложению.
 */
void ChatServer_Destroy(ChatServer *server);

/** @brief Обслужить транспорт, кадры, таймеры и обновления списка участников.
 * @param[in,out] server Живой сервер одного владельца.
 * @param[in] timeout_ms Ожидание 1..1000 мс.
 * @return 0 при успехе, -1 с кодом Winsock для ошибки самого цикла/аргументов.
 * @note Ошибочные команды и потоки обрабатываются отдельно для каждого сеанса.
 */
int ChatServer_Poll(ChatServer *server, int timeout_ms);

/** @brief Прочитать фактический порт listener.
 * @param[in] server Живой объект владельца либо NULL.
 * @return Порт хоста или 0 для NULL; владение не меняется.
 */
u_short ChatServer_Port(const ChatServer *server);
#endif
