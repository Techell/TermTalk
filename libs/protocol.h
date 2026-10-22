#ifndef WINSOCK_PROTOCOL_H
#define WINSOCK_PROTOCOL_H
/** @file protocol.h
 * @brief Ограниченный бинарный кодек WSCK v2 без зависимости от сокетов и UI.
 */
#include <stddef.h>
#include <stdint.h>

/** Размер фиксированного заголовка. */
#define PROTOCOL_HEADER_SIZE 16
/** Максимальный UTF-8 текст личного сообщения, в байтах. */
#define PROTOCOL_TEXT_MAX 4096
/** Максимальный UTF-8 никнейм, в байтах. */
#define PROTOCOL_NICK_MAX 32
/** Максимум записей списка участников. */
#define PROTOCOL_USERS_MAX 32
/** Наибольший payload реализованных команд: FILE_DATA. */
#define PROTOCOL_PAYLOAD_MAX (PROTOCOL_TEXT_MAX + 12)
/** Ёмкость одного полного кадра; не требует динамического выделения. */
#define PROTOCOL_FRAME_MAX (PROTOCOL_HEADER_SIZE + PROTOCOL_PAYLOAD_MAX)
/** Абсолютный срок HELLO и сборки неполного кадра, миллисекунды. */
#define PROTOCOL_TIMEOUT_MS 10000
/** Версия расширенного сетевого формата. */
#define PROTOCOL_VERSION 2
/** Размер бинарного блока; максимальный payload остаётся 4108 байт. */
#define PROTOCOL_FILE_BLOCK 4096
/** Абсолютный предел формата: 1 ТиБ; сервер может назначить меньший. */
#define PROTOCOL_FILE_MAX (UINT64_C(1024) * 1024 * 1024 * 1024)
/** Серверный предел одной передачи по умолчанию: 32 ГиБ. */
#define PROTOCOL_FILE_DEFAULT (UINT64_C(32) * 1024 * 1024 * 1024)
/** Предел UTF-8 имени в предложении, без пути. */
#define PROTOCOL_FILE_NAME_MAX 128

/** Команды и события WSCK v2; направление проверяет прикладной модуль. */
typedef enum {
    PROTO_HELLO = 0x01, /**< Регистрация никнейма. */
    PROTO_WELCOME = 0x02, /**< Назначенный ID. */
    PROTO_LIST_REQUEST = 0x03, /**< Запрос списка. */
    PROTO_LIST_RESPONSE = 0x04, /**< Снимок списка; request=0 для обновления. */
    PROTO_CHAT_SEND = 0x10, /**< Адресат и текст. */
    PROTO_CHAT_EVENT = 0x11, /**< Отправитель, исходный запрос и текст. */
    PROTO_ACCEPTED = 0x12, /**< Поставлено в очередь сервера. */
    PROTO_PING = 0x20, /**< Запрос отклика. */
    PROTO_PONG = 0x21, /**< Отклик. */
    PROTO_STATUS_REQUEST = 0x22, /**< Анонимная либо активная проверка сервера. */
    PROTO_STATUS_RESPONSE = 0x23, /**< Число участников и ёмкость. */
    PROTO_GOODBYE = 0x30, /**< Завершение сеанса. */
    PROTO_FILE_OFFER = 0x40, /**< Адресат, размер и имя. */
    PROTO_FILE_OFFER_EVENT = 0x41, /**< ID передачи, отправитель, размер, имя. */
    PROTO_FILE_CREATED = 0x42, /**< Назначенный transfer ID и адресат. */
    PROTO_FILE_DECISION = 0x43, /**< Согласие/отказ получателя. */
    PROTO_FILE_DECISION_EVENT = 0x44, /**< Ответ получателя отправителю. */
    PROTO_FILE_DATA = 0x45, /**< Ограниченный бинарный блок с offset. */
    PROTO_FILE_ACK = 0x46, /**< Следующий offset записанных байтов. */
    PROTO_FILE_END = 0x47, /**< Конец и SHA-256. */
    PROTO_FILE_DONE = 0x48, /**< Проверенный файл сохранён получателем. */
    PROTO_FILE_CANCEL = 0x49, /**< Отмена передачи и причина. */
    PROTO_ERROR = 0x7f /**< Числовая ошибка и описание. */
} ProtocolType;

/** Числовые ошибки прикладного протокола, независимые от Winsock. */
typedef enum {
    PROTO_BAD_STATE = 1, /**< Команда запрещена в состоянии сеанса. */
    PROTO_BAD_PAYLOAD = 2, /**< Неверное содержимое/направление. */
    PROTO_NICK_IN_USE = 3, /**< Никнейм занят. */
    PROTO_RECIPIENT_UNAVAILABLE = 4, /**< Адресат отсутствует или совпадает с источником. */
    PROTO_QUEUE_FULL = 5, /**< Нет места для целого события и подтверждения. */
    PROTO_SERVER_FULL = 6, /**< Сервер не может зарегистрировать участника. */
    PROTO_BAD_REQUEST_ID = 7, /**< Нулевой или не возрастающий ID запроса. */
    PROTO_BAD_TRANSFER = 8 /**< Передача занята, неизвестна или нарушен её порядок. */
} ProtocolError;

/** Заимствованный кадр; payload живёт только во время callback. */
typedef struct {
    uint8_t type; /**< ProtocolType. */
    uint32_t request_id; /**< ID запроса или 0 для самостоятельного события. */
    const unsigned char *payload; /**< Не C-строка; не освобождать. */
    size_t length; /**< Число байтов payload. */
} ProtocolFrame;

/** Собственная копия записи списка; status=1 означает подключённого участника. */
typedef struct {
    uint32_t id; /**< Ненулевой ID, не переиспользуется до перезапуска сервера. */
    unsigned char status; /**< В версии 1 только 1 (online). */
    char nickname[PROTOCOL_NICK_MAX + 1]; /**< Проверенный UTF-8 с локальным NUL. */
    unsigned char ipv4[4]; /**< Сетевой порядок четырёх октетов. */
    uint16_t port; /**< Порт TCP-клиента в порядке хоста. */
} ProtocolParticipant;

/** Инкрементальный накопитель одного потока; один владелец, не копировать в работе. */
typedef struct {
    unsigned char bytes[PROTOCOL_FRAME_MAX]; /**< Фиксированное хранилище кадра. */
    size_t used; /**< Число накопленных байтов. */
    size_t target; /**< 16 до чтения заголовка, затем полный размер. */
    unsigned long frames_started; /**< Счётчик начал кадров для внешнего абсолютного таймера. */
    int failed; /**< Ошибка необратима до явного Init. */
} ProtocolDecoder;

/** @brief Обработать целый проверенный кадр.
 * @param[in] frame Заимствованный кадр до возврата callback.
 * @param[in,out] context Заимствованный контекст владельца декодера.
 * @retval 0 Продолжить; -1 остановить разбор с ошибкой.
 * @note Не вызывать Feed/Init рекурсивно для того же декодера.
 */
typedef int (*ProtocolHandler)(const ProtocolFrame *frame, void *context);

/** @brief Сбросить накопитель. @param[out] decoder Ненулевой объект без параллельного доступа.
 * @note Ресурсами не владеет; стирает незавершённый кадр.
 */
void Protocol_Init(ProtocolDecoder *decoder);

/** @brief Добавить произвольный фрагмент, выделяя все полные кадры по порядку.
 * @param[in,out] decoder Инициализированный накопитель одного владельца.
 * @param[in] data Доступные length байт; NULL допустим только для нуля.
 * @param[in] length Размер фрагмента, может включать несколько кадров.
 * @param[in] handler Обязательный callback; получает заимствованное содержимое.
 * @param[in,out] context Заимствованный пользовательский контекст либо NULL.
 * @retval 0 Успех, включая неполный хвост; -1 ошибка формата или callback.
 * @note Не выделяет память по сетевой длине; ошибочный поток далее не разбирается.
 */
int Protocol_Feed(ProtocolDecoder *decoder, const void *data, size_t length,
                  ProtocolHandler handler, void *context);

/** @brief Проверить допустимость конца потока.
 * @param[in] decoder Инициализированный накопитель одного владельца.
 * @return 1 для чистой границы кадров, иначе 0 (оборванный/ошибочный кадр).
 * @note Ничего не освобождает и не меняет.
 */
int Protocol_Finished(const ProtocolDecoder *decoder);

/** @brief Закодировать целый кадр с проверкой синтаксиса payload.
 * @param[out] output Доступный буфер capacity байт, не пересекающийся с payload.
 * @param[in] capacity Вместимость output.
 * @param[in] type ProtocolType. @param[in] request_id ID в порядке хоста.
 * @param[in] payload Доступные length байт; NULL допустим для нуля.
 * @param[in] length Размер payload.
 * @return Полный размер кадра либо 0 при ошибке; буфер при ошибке не меняется.
 * @note Источник копируется; владение буферами остаётся у вызывающего.
 */
size_t Protocol_Encode(void *output, size_t capacity, uint8_t type, uint32_t request_id,
                       const void *payload, size_t length);

/** @brief Проверить UTF-8 без NUL, недопустимых скаляров и управляющих байтов.
 * @param[in] data Доступные length байт. @param[in] length Размер строки без NUL.
 * @param[in] nickname Ненулевое запрещает все C0; ноль разрешает TAB и LF.
 * @return 1 при корректной строке, иначе 0; пустая строка разрешена этой функцией.
 * @note Ограничения длины задаёт вызывающий; владение не меняется.
 */
int Protocol_ValidText(const unsigned char *data, size_t length, int nickname);

/** @brief Прочитать запись списка с проверкой границ.
 * @param[in] data Payload списка. @param[in] length Размер payload.
 * @param[in,out] offset Смещение начала записи; изменяется только при успехе.
 * @param[out] participant Собственная копия записи, только при успехе.
 * @return 1 при успехе, иначе 0. @note Не выделяет память; один владелец выходов.
 */
int Protocol_ReadParticipant(const unsigned char *data, size_t length, size_t *offset,
                              ProtocolParticipant *participant);

/** @brief Прочитать big-endian u16. @param[in] p Доступные 2 байта.
 * @return Значение хоста. @note Память заимствована, границы проверяет вызывающий.
 */
uint16_t Protocol_Read16(const unsigned char *p);
/** @brief Прочитать big-endian u32. @param[in] p Доступные 4 байта.
 * @return Значение хоста. @note Память заимствована, границы проверяет вызывающий.
 */
uint32_t Protocol_Read32(const unsigned char *p);
/** @brief Записать big-endian u16. @param[out] p Доступные 2 байта.
 * @param[in] value Значение хоста. @note Один владелец буфера; без передачи владения.
 */
void Protocol_Write16(unsigned char *p, uint16_t value);
/** @brief Записать big-endian u32. @param[out] p Доступные 4 байта.
 * @param[in] value Значение хоста. @note Один владелец буфера; без передачи владения.
 */
void Protocol_Write32(unsigned char *p, uint32_t value);
/** @brief Прочитать big-endian u64. @param p Заимствованные доступные 8 байт.
 * @return Значение хоста; границы проверяет вызывающий. */
uint64_t Protocol_Read64(const unsigned char *p);
/** @brief Записать big-endian u64. @param p Доступные 8 байт, один владелец.
 * @param value Значение хоста; владение буфером не меняется. */
void Protocol_Write64(unsigned char *p, uint64_t value);
#endif
