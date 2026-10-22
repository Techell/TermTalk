#ifndef WINSOCK_SERVER_PROBE_H
#define WINSOCK_SERVER_PROBE_H
/** @file server_probe.h @brief Отменяемая фоновая проба WSCK, не регистрирующая пользователя. */
#include "network.h"
#include "server_book.h"
/** Фоновая проба владеет своим потоком, Client и Winsock-ссылкой. */
typedef struct ServerProbe ServerProbe;
/** Копия результата пробы; online подтверждён только правильным ответом WSCK. */
typedef struct {
    ServerAddress address; /**< Проверенный адрес. */
    int online; /**< Получен STATUS_RESPONSE. */
    unsigned capacity; /**< Настроенный предел участников. */
    uint64_t file_limit; /**< Серверный предел одного файла, байты; 0 запрещает передачу. */
    unsigned users; /**< Текущее число зарегистрированных участников. */
    unsigned latency; /**< RTT приложения после TCP connect, мс. */
    int error; /**< Winsock/протокол/таймер, 0 при успехе. */
} ServerProbeResult;
/** @brief Запустить пробу со сроком до 1,5 с; UI не ожидает сеть.
 * @param address Адрес копируется. @return Владеющий указатель либо NULL; требуется Destroy. */
ServerProbe *ServerProbe_Start(const ServerAddress *address);
/** @brief Прочитать завершённый результат с публикацией через Interlocked.
 * @param probe Живой объект. @param result Выходная копия. @return 1 готово, 0 работает. */
int ServerProbe_Read(ServerProbe *probe, ServerProbeResult *result);
/** @brief Отменить, дождаться и освободить пробу; её сокет закрывает создавший поток.
 * @param probe Владеющий указатель либо NULL; после возврата недействителен. */
void ServerProbe_Destroy(ServerProbe *probe);
#endif
