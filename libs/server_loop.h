#ifndef WINSOCK_SERVER_LOOP_H
#define WINSOCK_SERVER_LOOP_H

/** @file server_loop.h
 * @brief Однопоточный владелец неблокирующих соединений TCP-сервера.
 */
#include "network.h"

/** Число соединений по умолчанию для транспортного реестра. */
#define SERVER_CLIENT_LIMIT 10
/** Верхняя граница: 32 участника и отдельный слот проверки/регистрации. */
#define SERVER_CLIENT_CAPACITY 33
/** Предельное ожидание остатка TX после входящего EOF, миллисекунды. */
#define SERVER_DRAIN_TIMEOUT_MS 3000
/** Нет прогресса непустой очереди TX столько миллисекунд — отключаем получателя. */
#define SERVER_STALL_TIMEOUT_MS 5000

/** Непрозрачный реестр; не копируется, все операции выполняет один поток. */
typedef struct ServerLoop ServerLoop;

/** Уведомление жизненного цикла заимствованного соединения. */
typedef enum {
    SERVER_CONNECTION_OPEN, /**< Сокет передан слоту, допускается создание сеанса. */
    SERVER_CONNECTION_CLOSE /**< Сеанс должен освободить заимствования до возврата. */
} ServerConnectionEvent;

/** @brief Наблюдать создание и закрытие сокета в потоке владельца.
 * @param[in,out] client Заимствование живёт от OPEN до CLOSE включительно.
 * @param[in] event Событие жизненного цикла.
 * @param[in,out] context Заимствованный контекст до удаления наблюдателя/сервера.
 * @note Допускается хранить client между этими уведомлениями. Не закрывать сокет,
 *       не вызывать Poll/Disconnect/Destroy и не менять реестр из callback.
 */
typedef void (*ServerLifecycleHandler)(Client *client, ServerConnectionEvent event, void *context);

/** @brief Установить наблюдателя до первого подключения.
 * @param[in,out] loop Живой реестр одного владельца без активных клиентов.
 * @param[in] handler Наблюдатель либо NULL. @param[in,out] context Контекст либо NULL.
 * @return 0 при успехе, -1 с WSAEINVAL при ошибке/непустом реестре.
 * @note Владение сокетами остаётся у ServerLoop; старый контекст больше не используется.
 */
int ServerLoop_SetLifecycle(ServerLoop *loop, ServerLifecycleHandler handler, void *context);

/** @brief Запросить немедленное закрытие конкретного слота у владельца.
 * @param[in,out] loop Живой реестр одного владельца.
 * @param[in] client Заимствование из lifecycle, не использовать после вызова.
 * @return 0 при закрытии, -1 с WSAEINVAL для чужого/пустого слота.
 * @note Разрешено между Poll, запрещено из lifecycle. Не гарантирует доставку TX.
 */
int ServerLoop_Disconnect(ServerLoop *loop, Client *client);

/** Снимок счётчиков одного владельца; не является подтверждением доставки. */
typedef struct {
    unsigned active; /**< Число занятых соединений. */
    unsigned accepted; /**< Всего принятых соединений. */
    unsigned rejected; /**< Отклонено сверх лимита. */
    unsigned slow; /**< Отключено по переполнению или отсутствию прогресса TX. */
    unsigned waits; /**< Число вызовов select, для проверки отсутствия пустого цикла. */
} ServerLoopStats;

/**
 * @brief Обработать заимствованный фрагмент байтов в потоке сервера.
 * @param[in,out] loop Заимствованный сервер; разрешён Broadcast, запрещены Poll/Destroy.
 * @param[in,out] sender Заимствованное соединение; не закрывать и не сохранять указатель.
 * @param[in] data Буфер на length байт, живёт только до возврата обработчика.
 * @param[in] length Положительное число байтов; это не длина сообщения.
 * @param[in,out] context Пользовательский контекст, которым владеет вызывающий Poll.
 * @retval 0 Продолжить обслуживание.
 * @retval -1 Закрыть отправителя после возврата обработчика.
 * @note Не блокировать поток: файл, шифрование и ожидания выполняются отдельно.
 */
typedef int (*ServerDataHandler)(ServerLoop *loop, Client *sender,
                                 const char *data, int length, void *context);

/**
 * @brief Создать реестр и неблокирующий слушающий сокет.
 * @param[in] port Порт хоста, 0 — выбрать автоматически.
 * @pre Winsock запущен; создающий поток становится единственным владельцем.
 * @return Владеющий указатель либо NULL с причиной в WSAGetLastError.
 * @note Ресурсы освобождает ServerLoop_Destroy; частичная ошибка очищается внутри.
 */
ServerLoop *ServerLoop_Create(u_short port);
/** @brief Создать транспорт с заданным числом слотов.
 * @param port Порт хоста, 0 для автоматического. @param limit Число слотов 1..33.
 * @return Владеющий указатель либо NULL с Winsock-ошибкой, WSAEINVAL при неверном лимите.
 * @pre Winsock запущен; единственный владелец — вызывающий поток.
 * @note Освобождать Destroy, сокеты не передаются прикладному модулю. */
ServerLoop *ServerLoop_CreateConfigured(u_short port, unsigned limit);

/**
 * @brief Закрыть listener и всех клиентов, освободить реестр.
 * @param[in,out] loop Владеющий указатель либо NULL; после вызова недействителен.
 * @pre Нет параллельных операций и активного callback; вызывает владелец.
 * @note Не ждёт доставки TX. Winsock остаётся собственностью приложения.
 */
void ServerLoop_Destroy(ServerLoop *loop);

/**
 * @brief Выполнить ограниченный проход обслуживания с ожиданием готовности.
 * @param[in,out] loop Живой реестр единственного владельца.
 * @param[in] timeout_ms Максимальное ожидание, 1..1000 мс; таймеры проверяются каждый проход.
 * @param[in] handler Обязательный обработчик фрагментов, не вызывается рекурсивно.
 * @param[in,out] context Заимствованный контекст либо NULL.
 * @retval 0 Проход выполнен, включая тайм-аут и локальные отключения клиентов.
 * @retval -1 Неверные параметры или ошибка listener/select; сохранить WSAGetLastError.
 * @note На клиента — один recv и одна попытка send; accept не более лимита за проход.
 *       Ошибка одного клиента не останавливает сервер. Пустые TX не входят в write-набор.
 */
int ServerLoop_Poll(ServerLoop *loop, int timeout_ms, ServerDataHandler handler, void *context);

/**
 * @brief Поставить байты в очереди всех OPEN-клиентов, кроме отправителя.
 * @param[in,out] loop Реестр единственного владельца, допускается из handler.
 * @param[in] sender Соединение из handler либо NULL для отправки всем.
 * @param[in] data Источник length байт; копируется, не должен пересекаться с TX.
 * @param[in] length Число байтов 0..NETWORK_TX_CAPACITY.
 * @return Число очередей, принявших байты; -1 при неверном аргументе.
 * @note Переполненный получатель отключается; другие продолжают работать.
 *       Это служебная рассылка байтов без команд, адресации и гарантии доставки.
 */
int ServerLoop_Broadcast(ServerLoop *loop, const Client *sender, const void *data, int length);

/** @brief Прочитать выбранный порт.
 * @param[in] loop Живой реестр владельца.
 * @return Порт хоста; 0 для NULL. Владение не меняется.
 */
u_short ServerLoop_Port(const ServerLoop *loop);

/** @brief Прочитать счётчики без передачи владения.
 * @param[in] loop Живой реестр; вызов только из потока владельца.
 * @return Копия счётчиков, нулевая для NULL.
 */
ServerLoopStats ServerLoop_GetStats(const ServerLoop *loop);

#endif
