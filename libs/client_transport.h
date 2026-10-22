#ifndef WINSOCK_CLIENT_TRANSPORT_H
#define WINSOCK_CLIENT_TRANSPORT_H
/** @file client_transport.h @brief Единственный сетевой работник и ограниченные очереди команд/кадров. */
#include "network.h"
#include "protocol.h"
/** @brief Непрозрачный объект владеет потоком/очередями, но заимствует Client до уничтожения. */
typedef struct ClientTransport ClientTransport;
/** @brief Копия кадра: payload принадлежит значению, никаких указателей в декодер. */
typedef struct {
    uint8_t type; /**< Тип кадра WSCK v2. */
    uint32_t request; /**< ID запроса либо 0 у уведомлений. */
    size_t length; /**< Число действующих байтов payload. */
    unsigned char payload[PROTOCOL_PAYLOAD_MAX]; /**< Собственная ограниченная копия полезной нагрузки. */
} ClientEvent;
/** @brief Создать поток; сокет остаётся у вызывающего.
 * @param client Подключённый Client, живущий до Transport_Destroy.
 * @return Владеющий указатель или NULL. @pre Другие потоки не вызывают I/O для client. */
ClientTransport *Transport_Create(Client *client);
/** @brief Атомарно поставить целый кадр в очередь (32 элемента), без ожидания сети.
 * @param transport Действующий объект. @param type Тип. @param request Ненулевой возрастающий ID.
 * @param payload Заимствованные байты либо NULL при length=0. @param length Длина.
 * @return 1 принят, 0 неверные данные/очередь заполнена/сеанс завершён; повторять по решению UI. */
int Transport_Post(ClientTransport *transport, uint8_t type, uint32_t request, const void *payload, size_t length);
/** @brief Забрать копию входного кадра из очереди (64 элемента).
 * @param transport Объект. @param event Выходная копия вызывающего. @return 1 кадр, 0 очередь пуста. */
int Transport_Pop(ClientTransport *transport, ClientEvent *event);
/** @brief Запросить EOF отправки после обработки уже поставленных команд.
 * @param transport Объект; дальнейшие Post запрещены. Не закрывает сокет. */
void Transport_Finish(ClientTransport *transport);
/** @brief Проверить завершение и опубликованный результат потока.
 * @param transport Объект. @param error Код Winsock либо 0 при штатном EOF; заполняется при завершении.
 * @return 1 поток завершён, 0 ещё работает. */
int Transport_Done(ClientTransport *transport, int *error);
/** @brief Остановить, дождаться работника и освободить ресурсы.
 * @param transport Владеющий указатель либо NULL; после возврата недействителен.
 * @note Client и сокет не уничтожаются; теперь их можно закрыть вызывающему. */
void Transport_Destroy(ClientTransport *transport);
#endif
