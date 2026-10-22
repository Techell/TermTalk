#ifndef WINSOCK_NETWORK_H
#define WINSOCK_NETWORK_H

/**
 * @file network.h
 * @brief Сокеты, соединение и ограниченная очередь байтов.
 * @details Модуль не разбирает команды чата и не обращается к интерфейсу.
 */

/* Winsock подключается раньше заголовков, способных включить windows.h. */
#include <winsock2.h>
#include <ws2tcpip.h>

/** Вместимость текстового IPv4-адреса вместе с завершающим NUL. */
#define MAX_IP_SIZE 16
/** Вместимость сетевого фрагмента RX; UI задаёт свой предел ввода отдельно. */
#define INPUT_BUF_MAX 1024
/** Вместимость ограниченной очереди TX; не является лимитом строки или кадра. */
#define NETWORK_TX_CAPACITY (64 * 1024)
/** Порт учебного сервиса по умолчанию. */
#define DEFAULT_PORT 5050
/** Результат Socket_Recv: данных пока нет, соединение не закрыто. */
#define SOCKET_RECV_WOULD_BLOCK (-2)
/** Очередь приложения заполнена; данные не добавлены, соединение сохранено. */
#define SOCKET_QUEUE_FULL (-3)

/** Состояние транспорта; не является регистрацией пользователя или проверкой связи. */
typedef enum {
    CLIENT_CLOSED = 0, /**< Ресурсов нет. */
    CLIENT_CONNECTING, /**< Выполняется подключение. */
    CLIENT_OPEN, /**< Разрешены приём и добавление данных. */
    CLIENT_DRAINING, /**< Отправляется остаток; новые данные запрещены. */
    CLIENT_SEND_CLOSED, /**< Отправка завершена, приём разрешён. */
    CLIENT_FAILED /**< Ошибка транспорта; требуется закрытие владельцем. */
} ClientState;

/** Категория результата Winsock; исходный код ошибки сохраняется отдельно вызывающим. */
typedef enum {
    NET_ERROR_NONE, /**< Ошибки нет. */
    NET_ERROR_WAIT, /**< Повтор после готовности, для connect — ожидание результата. */
    NET_ERROR_REFUSED, /**< Подключение отклонено. */
    NET_ERROR_UNREACHABLE, /**< Нет доступного пути к сети или узлу. */
    NET_ERROR_TIMEOUT, /**< Системный тайм-аут. */
    NET_ERROR_DISCONNECTED, /**< Сброс или прерывание соединения. */
    NET_ERROR_LOCAL, /**< Причину уточняет Network_ClassifyLocalError. */
    NET_ERROR_SYSTEM /**< Неизвестная либо общесистемная ошибка. */
} NetworkError;

/** Ошибки локальных параметров, состояния и настройки ресурсов. */
typedef enum {
    NET_LOCAL_NONE, /**< Локальная категория не определена. */
    NET_LOCAL_ARGUMENT, /**< Неверный параметр или адрес памяти. */
    NET_LOCAL_STATE, /**< Неверный дескриптор или состояние операции. */
    NET_LOCAL_ADDRESS_IN_USE, /**< Адрес уже занят. */
    NET_LOCAL_ADDRESS_UNAVAILABLE, /**< Локальный адрес недоступен. */
    NET_LOCAL_ACCESS, /**< Операция запрещена системой. */
    NET_LOCAL_RESOURCES, /**< Не хватает системных буферов или дескрипторов. */
    NET_LOCAL_NOT_INITIALIZED /**< Winsock не запущен или версия не поддерживается. */
} NetworkLocalError;

/** Инициализатор пустого объекта; обязателен до первого вызова инициализации. */
#define CLIENT_INITIALIZER { .socket = INVALID_SOCKET }

/**
 * @brief Ресурсы одного соединения, а не постоянная личность пользователя.
 * @invariant 0 <= tx_size && tx_size <= NETWORK_TX_CAPACITY.
 * @warning Не копировать объект для создания второго владельца сокета.
 * @pre До первого использования задать CLIENT_INITIALIZER. Инициализация и
 *      закрытие выполняются владельцем без параллельных операций.
 * @note Очередью отправки владеет один поток. Закрытие — после завершения
 *       всех операций других потоков с этим сокетом. Право владения является
 *       контрактом C API: идентификатор потока не проверяется автоматически.
 */
typedef struct
{
    SOCKET socket;                    /**< Сокет или INVALID_SOCKET после сброса. */
    volatile LONG state;              /**< ClientState; читать через Client_GetState, не менять напрямую. */
    volatile LONG peer_eof;           /**< Конец входящего потока; читать через Client_HasPeerEOF. */
    char ip_connected[MAX_IP_SIZE];    /**< Собственная копия IPv4-адреса собеседника. */
    u_short port_connected;            /**< Порт собеседника в порядке байтов хоста. */
    char rx_buffer[INPUT_BUF_MAX];     /**< Резерв для будущего накопителя приёма; сейчас не используется. */
    int rx_size;                      /**< Резерв счётчика RX; текущий приём его не меняет. */
    char tx_buffer[NETWORK_TX_CAPACITY]; /**< Байты, ожидающие передачи через Socket_Flush. */
    int tx_size;                      /**< Число ожидающих байтов, не длина C-строки. */
} Client;

/** @brief Прочитать состояние атомарно.
 * @param[in] client Живой объект; NULL трактуется как CLOSED.
 * @return Снимок ClientState, не гарантия доступности собеседника.
 * @note Не передаёт владение; объект нельзя закрывать во время вызова.
 */
ClientState Client_GetState(Client *client);

/** @brief Проверить завершение отправки собеседником.
 * @param[in] client Живой объект либо NULL.
 * @return Ненулевое значение после входящего EOF; для NULL — 0.
 * @note Приём имеет одного владельца; состояние читается атомарно.
 */
int Client_HasPeerEOF(Client *client);

/** @brief Запретить новые данные и начать завершение отправки.
 * @param[in,out] client Объект OPEN/DRAINING; вызов владельцем TX.
 * @retval 0 Успех, включая повтор для DRAINING.
 * @retval -1 Неверное состояние/параметр; код в WSAGetLastError.
 * @note Не закрывает сокет и не отправляет очередь; владение не меняется.
 */
int Client_BeginDrain(Client *client);

/** @brief Завершить отправку после опустошения очереди.
 * @param[in,out] client Объект DRAINING с пустой очередью; вызов владельцем TX.
 * @retval 0 Успех, включая повтор для SEND_CLOSED.
 * @retval -1 Ошибка состояния либо shutdown; код в WSAGetLastError.
 * @note Приём остаётся разрешён; сокет закрывает прежний владелец.
 */
int Client_ShutdownSend(Client *client);

/** @brief Классифицировать сохранённый код Winsock без побочных эффектов.
 * @param[in] error Код WSAGetLastError/WSAStartup, 0 при успехе.
 * @return Категория ошибки; WAIT зависит от операции (connect требует завершения).
 * @note Не владеет ресурсами; не выполняет автоматического повтора.
 */
NetworkError Network_ClassifyError(int error);

/** @brief Выделить локальную причину из кода Winsock.
 * @param[in] error Сохранённый системный код.
 * @return Локальная категория либо NET_LOCAL_NONE.
 * @note Не меняет состояние и не передаёт владение.
 */
NetworkLocalError Network_ClassifyLocalError(int error);

/** @brief Получить краткое диагностическое описание ошибки.
 * @param[in] error Сохранённый код Winsock.
 * @return Заимствованная неизменяемая строка со статическим временем жизни.
 * @note Потокобезопасна; неизвестный числовой код нужно также выводить в журнал.
 */
const char *Network_ErrorText(int error);

/**
 * @brief Инициализировать Winsock 2.2 для процесса.
 * @retval 0 Успех; требуется последующий Network_Cleanup.
 * @return При ошибке — код, возвращённый WSAStartup, а не WSAGetLastError.
 */
int Network_Initialize(void);

/**
 * @brief Завершить использование Winsock текущим приложением.
 * @pre Network_Initialize завершился успешно; сокеты уже закрыты.
 * @note Вызывать один раз на успешную инициализацию, после остановки потоков.
 */
void Network_Cleanup(void);

/**
 * @brief Подключиться по IPv4 с FIONBIO до connect и ожиданием до 3 секунд.
 * @param[in,out] client Пустой объект CLIENT_INITIALIZER либо после Client_Close.
 * @param[in] ip_server NUL-терминированный IPv4-адрес; копируется в client.
 * @param[in] port_server Порт сервера в порядке байтов хоста, 1..65535.
 * @pre Winsock инициализирован; указатели ненулевые.
 * @retval 0 Соединение создано, client владеет его сокетом.
 * @retval -1 Ошибка; код доступен через WSAGetLastError.
 * @note Обёртка ConnectTimed без отмены. Закрывать прежний сокет нужно до вызова;
 *       частичная ошибка очищает созданный здесь ресурс, причина сохраняется.
 */
int Client_Initialize(Client *client, const char *ip_server, u_short port_server);
/** @brief Создать соединение с ограниченным ожиданием и отменой.
 * @param client Закрытый объект владельца, живёт до Client_Close.
 * @param ip_server IPv4 с NUL, заимствован на время вызова. @param port_server Порт 1..65535.
 * @param timeout_ms Срок 1..30000 мс. @param cancel Заимствованное событие отмены либо NULL.
 * @return 0 подключено, -1 с причиной Winsock, включая WSAETIMEDOUT/WSAEINTR.
 * @pre Winsock запущен; только один поток обращается к объекту.
 * @note Создавший поток закрывает сокет. При ошибке ресурс уже закрыт, при успехе сокет неблокирующий. */
int Client_ConnectTimed(Client *client, const char *ip_server, u_short port_server, unsigned timeout_ms, HANDLE cancel);

/**
 * @brief Создать неблокирующий слушающий TCP/IPv4-сокет на всех интерфейсах.
 * @param[in,out] listener Указатель на INVALID_SOCKET; получает владение при успехе.
 * @param[in] port Порт в порядке хоста; 0 поручает системе выбрать свободный порт.
 * @pre Winsock инициализирован; настройка выполняется единственным владельцем.
 * @retval 0 Успех; вызывающий обязан вызвать closesocket после окончания работы.
 * @retval -1 Ошибка; исходный объект не изменён, причина в WSAGetLastError.
 * @note При неудаче промежуточный сокет закрывается внутри функции.
 */
int Server_Listen(SOCKET *listener, u_short port);

/**
 * @brief Выполнить одну попытку принять соединение без ожидания.
 * @param[in] listener Неблокирующий слушающий сокет; владение не меняется.
 * @param[in,out] client Пустой CLIENT_INITIALIZER либо объект после Client_Close.
 * @pre Winsock инициализирован; клиент и listener используются единственным владельцем.
 * @retval 0 Принят неблокирующий сокет, владение передано client.
 * @retval 1 Подключений пока нет (WSAEWOULDBLOCK); повторить после готовности.
 * @retval -1 Ошибка; клиент не получил ресурс, код в WSAGetLastError.
 * @note Сокет клиента независим от listener и закрывается через Client_Close.
 */
int Server_Accept(SOCKET listener, Client *client);

/**
 * @brief Закрыть сокет соединения и сбросить его поля.
 * @param[in,out] client Инициализированный объект либо NULL (тогда нет действия).
 * @pre Другие потоки больше не используют сокет; объект не содержит мусорный HANDLE.
 * @note Повторный вызов для уже сброшенного объекта допустим.
 */
void Client_Close(Client *client);

/**
 * @brief Добавить все переданные байты в ограниченную локальную очередь.
 * @param[in,out] client Соединение OPEN с допустимым tx_size.
 * @param[in] data Источник байтов; NULL допустим только при length == 0.
 * @param[in] length Число байтов; неотрицательное, без неявного добавления NUL.
 * @pre Память data доступна на length байт и не пересекается с областью копирования.
 * @pre Вызов выполняет единственный владелец очереди отправки.
 * @retval 0 Данные целиком скопированы в очередь; исходный буфер можно менять.
 * @retval -1 Данные не добавлены; WSAEINVAL при неверных аргументах,
 *            WSAENOTCONN при состоянии, отличном от OPEN.
 * @retval SOCKET_QUEUE_FULL Недостаточно места в очереди приложения; это не ошибка Winsock.
 * @note Функция не вызывает send. Для передачи нужно вызывать Socket_Flush,
 *       пока tx_size > 0. Принятие в очередь не подтверждает доставку.
 */
int Socket_Send(Client *client, const void *data, int length);

/**
 * @brief Выполнить одну попытку отправки и сохранить непереданный остаток.
 * @param[in,out] client Соединение OPEN/DRAINING и очередь одного потока отправки.
 * @pre Для неблокирующего поведения сокет должен быть переведён в FIONBIO.
 * @retval 0 Локальная очередь пуста.
 * @retval 1 Есть остаток либо WSAEWOULDBLOCK; дождаться готовности и повторить.
 * @retval -1 Ошибка, уточняемая через WSAGetLastError.
 * @note Внутри нет цикла ожидания. Успешный send не является клиентским ACK.
 */
int Socket_Flush(Client *client);

/**
 * @brief Прочитать доступный фрагмент байтового потока.
 * @param[in,out] client Соединение OPEN/DRAINING/SEND_CLOSED; один поток приёма.
 * @param[out] buffer Доступная для записи память минимум на buffer_size байт.
 * @param[in] buffer_size Положительная вместимость назначения в байтах.
 * @return Положительное число — количество прочитанных байтов.
 * @retval 0 Собеседник завершил отправку; доступные данные уже прочитаны.
 * @retval SOCKET_RECV_WOULD_BLOCK Пока нет данных на неблокирующем сокете.
 * @retval -1 Ошибка; использовать WSAGetLastError.
 * @note EOF устанавливает peer_eof; остальные ошибки, кроме ожидания,
 *       устанавливают FAILED без закрытия сокета. Начатая параллельная операция
 *       может завершиться после этого; владелец закрывает сокет после её окончания.
 * @warning NUL не добавляется. Один фрагмент не обязательно равен сообщению.
 */
int Socket_Recv(Client *client, char *buffer, int buffer_size);

/**
 * @brief Проверить десятичную запись порта без знака и посторонних символов.
 * @param[in] text NUL-терминированная строка цифр.
 * @param[out] port Получает значение в порядке байтов хоста только при успехе.
 * @retval 0 Значение находится в диапазоне 1..65535.
 * @retval -1 Неверный ввод/указатель; прежнее значение port не меняется.
 * @note Winsock для этого преобразования не требуется.
 */
int Parse_Port(const char *text, u_short *port);

#endif
