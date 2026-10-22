#ifndef WINSOCK_FILE_BROWSER_H
#define WINSOCK_FILE_BROWSER_H
/** @file file_browser.h @brief Ограниченный файловый адаптер и копируемые снимки для однопанельного UI. */
#include <stddef.h>
#include <stdint.h>
/** Предел записей снимка; при превышении доступен прямой ввод пути. */
#define BROWSER_ENTRIES_MAX 512
/** Вместимость пути UTF-8, включая NUL. */
#define BROWSER_PATH_MAX 3072
/** Запись снимка; пути и файлы не открывает. */
typedef struct {
    char name[1024]; /**< Собственная UTF-8 копия имени либо имени диска. */
    uint64_t size; /**< Размер файла, у каталога 0. */
    int directory; /**< 1 каталог/диск, 0 файл. */
} BrowserEntry;
/** Копия результата фонового перечисления; UI владеет своим экземпляром. */
typedef struct {
    char directory[BROWSER_PATH_MAX]; /**< Абсолютный каталог; пусто для списка дисков. */
    BrowserEntry entries[BROWSER_ENTRIES_MAX]; /**< Каталоги первыми, затем файлы. */
    unsigned count; /**< Число записей; не более BROWSER_ENTRIES_MAX. */
    int truncated; /**< Снимок усечён: используйте прямой путь для отсутствующих записей. */
    unsigned long error; /**< Win32-код перечисления, 0 при успехе. */
} BrowserListing;
/** Непрозрачный адаптер одного владельца; перечисляет каталог в своём потоке. */
typedef struct FileBrowser FileBrowser;
/** @brief Создать адаптер без запуска перечисления.
 * @return Владеющий указатель либо NULL; освобождает Browser_Destroy.
 * @note Не меняет текущий каталог процесса и не владеет сокетами. */
FileBrowser *Browser_Create(void);
/** @brief Запросить снимок без ожидания диска в UI.
 * @param browser Контекст основного потока. @param path UTF-8 каталог, NULL для дисков.
 * @return 1 запущен, 0 ещё работает предыдущий запрос, -1 ошибка аргумента/ресурса.
 * @note Строка копируется, повторный запрос разрешён после Read; отмена кооперативная. */
int Browser_Request(FileBrowser *browser, const char *path);
/** @brief Забрать готовый снимок с синхронизацией публикации и освободить поток.
 * @param browser Контекст. @param listing Выходная собственная копия.
 * @return 1 готово, 0 ещё работает/нет запроса. Никаких указателей на рабочий буфер не отдаёт. */
int Browser_Read(FileBrowser *browser, BrowserListing *listing);
/** @brief Запросить отмену, не ожидая файловую систему.
 * @param browser Живой контекст. @note Проверяется между WinAPI-операциями. */
void Browser_Cancel(FileBrowser *browser);
/** @brief Дождаться работника и освободить адаптер.
 * @param browser Владеющий указатель либо NULL. После возврата недействителен.
 * @note WinAPI-вызов файловой системы может задержать завершение; ресурс поиска закрывает работник. */
void Browser_Destroy(FileBrowser *browser);
/** @brief Получить каталог исполняемого клиента либо его downloads.
 * @param downloads Ненулевое добавляет downloads. @param output Буфер UTF-8 вызывающего.
 * @param capacity Его размер. @return 1 успех, 0 путь не помещается/ошибка WinAPI.
 * @note Не создаёт каталог и не меняет владение. */
int Browser_ClientDirectory(int downloads, char *output, size_t capacity);
/** @brief Получить родительский каталог.
 * @param directory Абсолютный UTF-8 путь. @param output Буфер вызывающего.
 * @param capacity Его размер. @return 1 успех; пустой результат означает список дисков.
 * @note Не меняет рабочий каталог процесса и исходную строку. */
int Browser_Parent(const char *directory, char *output, size_t capacity);
/** @brief Составить путь записи снимка без файлового I/O.
 * @param listing Неизменяемый снимок. @param index Индекс < count.
 * @param output Буфер вызывающего. @param capacity Его размер. @return 1 успех, 0 ошибка/не помещается. */
int Browser_EntryPath(const BrowserListing *listing, unsigned index, char *output, size_t capacity);
/** @brief Выбрать безопасное свободное имя входящего файла в каталоге.
 * @param directory Выбранный UTF-8 каталог. @param name Имя из предложения, без пути.
 * @param create_directory Разрешить создание самого каталога (для клиентского downloads).
 * @param output Буфер UTF-8 полного пути. @param capacity Его размер.
 * @return 1 найдено свободное имя, 0 ошибка; существующие файлы не меняются.
 * @note Недопустимые Windows-символы заменяются; коллизия получает суффикс (N).
 *       Это не резервирование: окончательный CREATE_NEW/rename выполняет FileTransfer. */
int Browser_ReceivePath(const char *directory, const char *name, int create_directory, char *output, size_t capacity);
#endif
