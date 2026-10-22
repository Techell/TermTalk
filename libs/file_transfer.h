#ifndef WINSOCK_FILE_TRANSFER_H
#define WINSOCK_FILE_TRANSFER_H
/** @file file_transfer.h @brief Потоковая передача файла, независимая от команд и консольных элементов. */
#include "protocol.h"
/** @brief Видимое состояние, не обещает доставки до FILE_COMPLETED. */
typedef enum {
    FILE_IDLE, FILE_OFFERING, FILE_WAITING, FILE_OFFERED, FILE_SENDING, FILE_RECEIVING,
    FILE_VERIFYING, FILE_COMPLETED, FILE_REFUSED, FILE_CANCELLED, FILE_FAILED
} FileStatus;
/** @brief Собственная копия метаданных; сетевые ID действуют только в текущем сеансе. */
typedef struct {
    FileStatus status; /**< Состояние предложения/передачи/результата. */
    uint32_t id; /**< ID передачи сервера либо 0 до назначения. */
    uint32_t peer; /**< ID второго участника. */
    uint64_t total; /**< Размер до 1 ТиБ, дополнительно ограничивается сервером. */
    uint64_t bytes; /**< Подтверждённые получателем байты у отправителя, записанные у получателя. */
    char name[PROTOCOL_FILE_NAME_MAX + 1]; /**< Проверенное UTF-8 имя, никогда не путь назначения. */
    unsigned long error; /**< Диагностический Win32-код либо причина отмены. */
    int outgoing; /**< 1 отправка, 0 приём. */
    int saved; /**< Получатель уже атомарно сохранил проверенный файл; отмена его не удаляет. */
} FileInfo;
/** @brief Поставить целый кадр без блокирующей сетевой операции.
 * @param context Заимствованный контекст приложения. @param type Тип запроса.
 * @param payload length байт до возврата, нужно скопировать. @param length Длина.
 * @param request Выходной ненулевой возрастающий ID при успехе.
 * @return 1 поставлен, 0 очередь заполнена: модуль сохранит подготовленный блок и повторит позже. */
typedef int (*FilePost)(void *context, uint8_t type, const void *payload, size_t length, uint32_t *request);
/** @brief Непрозрачный объект одного потока; владеет дескрипторами файла/хеша, не сокетом. */
typedef struct FileTransfer FileTransfer;
/** @brief Создать контекст с одной ограниченной передачей.
 * @param post Обязательная функция очереди. @param context Заимствован до Destroy.
 * @return Владеющий указатель либо NULL. Все методы вызывает основной поток. */
FileTransfer *FileTransfer_Create(FilePost post, void *context);
/** @brief Открыть источник и подготовить предложение.
 * @param transfer Живой контекст. @param recipient Ненулевой ID адресата.
 * @param path Путь UTF-8; внешняя пара кавычек разрешена, строка копируется.
 * @return 1 подготовлено, 0 занято/ошибка пути, размера или CNG; существующая передача не заменяется. */
int FileTransfer_Send(FileTransfer *transfer, uint32_t recipient, const char *path);
/** @brief Принять предложение в явно выбранный файл либо отказать.
 * @param transfer Контекст. @param id ID входящего предложения.
 * @param path Путь UTF-8 либо NULL для отказа; имя из сети не используется как путь.
 * @return 1 принято решение, 0 ошибка/неверный ID; существующий файл не перезаписывается. */
int FileTransfer_Decide(FileTransfer *transfer, uint32_t id, const char *path);
/** @brief Применить проверенный кодеком кадр текущего соединения, включая ERROR.
 * @param transfer Контекст. @param frame Заимствован до возврата; данные копируются/обрабатываются сразу.
 * @note Не меняет сокет и не рисует UI; неизвестные завершённые transfer ID игнорируются. */
void FileTransfer_Frame(FileTransfer *transfer, const ProtocolFrame *frame);
/** @brief Обслужить один блок, ограниченную повторную постановку и таймеры.
 * @param transfer Контекст. @note Вызывать регулярно; читает не более 4096 байт за проход. */
void FileTransfer_Tick(FileTransfer *transfer);
/** @brief Отменить свою передачу, закрыть файлы и удалить только собственный временный файл.
 * @param transfer Контекст. @param reason Причина WSCK 1..6; уведомление ставится по возможности. */
void FileTransfer_Cancel(FileTransfer *transfer, unsigned char reason);
/** @brief Получить заимствованное состояние до следующего вызова изменяющего метода.
 * @param transfer Живой контекст. @return Не освобождаемый указатель. */
const FileInfo *FileTransfer_Info(const FileTransfer *transfer);
/** @brief Проверить наличие незавершённой операции, включая отмену ещё не назначенного ID.
 * @param transfer Живой контекст. @return 1 занято, 0 можно начинать следующую. */
int FileTransfer_Busy(const FileTransfer *transfer);
/** @brief Освободить контекст после прекращения использования его callback.
 * @param transfer Владеющий указатель либо NULL. Удаляется только собственный незавершённый part-файл.
 * @note Не отправляет кадры, не удаляет подтверждённый файл, не закрывает сокет. */
void FileTransfer_Destroy(FileTransfer *transfer);
#endif
