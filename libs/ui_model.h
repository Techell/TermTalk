#ifndef WINSOCK_UI_MODEL_H
#define WINSOCK_UI_MODEL_H
/** @file ui_model.h @brief Независимое от консоли состояние личного чата и редактор UTF-8. */
#include "protocol.h"
#include "server_book.h"
#include "file_transfer.h"
#include "file_browser.h"
#include "command_catalog.h"
#include <time.h>

/** @brief Число сообщений в памяти и на диске; старейшие вытесняются. */
#define UI_HISTORY_MAX 256
/** @brief Вместимость редактора с запасом для команды /msg. */
#define UI_INPUT_MAX (PROTOCOL_TEXT_MAX + 64)
/** Максимум последних команд текущего подключения; на диск не записываются. */
#define UI_COMMAND_HISTORY_MAX 20
/** @brief Язык подписей; язык сообщений от него не зависит. */
typedef enum { UI_EN, /**< Английские подписи. */ UI_RU /**< Русские подписи. */ } UiLanguage;
/** @brief Результат отправки; ACCEPTED означает только очередь сервера. */
typedef enum {
    UI_PENDING, /**< Отправка ещё не получила ответа. */
    UI_ACCEPTED, /**< Принято в очередь сервера, доставка не подтверждена. */
    UI_FAILED, /**< Сервер отказал в отправке. */
    UI_RESTORED /**< Загружено с диска, прежний результат доставки неизвестен. */
} UiDelivery;
/** @brief Сообщение владеет копиями строк; peer — ник, id действует лишь в текущем сеансе. */
typedef struct {
    char peer[PROTOCOL_NICK_MAX + 1]; /**< Копия UTF-8 ника собеседника либо числовая метка, если ник неизвестен. */
    char text[PROTOCOL_TEXT_MAX + 1]; /**< Копия проверенного UTF-8 текста с NUL. */
    uint32_t id; /**< Временный ID; 0 у восстановленных записей. */
    uint32_t request; /**< ID исходящего запроса текущего сеанса, не сохраняется. */
    time_t timestamp; /**< Локальное время получения/отправки, секунды Unix. */
    int outgoing; /**< 1 исходящее, 0 входящее. */
    UiDelivery delivery; /**< Результат постановки исходящего сообщения. */
} UiMessage;
/** @brief Модель принадлежит основному потоку; сеть и хранилище не удерживают её адрес.
 * @invariant count <= PROTOCOL_USERS_MAX, message_count <= UI_HISTORY_MAX.
 * @invariant Строки имеют NUL, текст — проверенный UTF-8; cursor на границе символа input.
 * @note Обнулить перед использованием; из-за размера выделять в куче, не в стеке. */
typedef struct {
    UiLanguage language; /**< Язык интерфейса. */
    char nickname[PROTOCOL_NICK_MAX + 1]; /**< Собственный UTF-8 ник. */
    char endpoint[128]; /**< Адрес сервера и порт, область локального профиля. */
    ProtocolParticipant users[PROTOCOL_USERS_MAX]; /**< Копия текущего снимка участников. */
    unsigned count; /**< Число действующих участников. */
    unsigned capacity; /**< Настроенный предел сервера, 0 до STATUS. */
    uint64_t file_limit; /**< Предел одного файла сервера, 0 запрещает передачу. */
    unsigned focus; /**< Индекс подсвеченной строки списка. */
    unsigned unread[PROTOCOL_USERS_MAX]; /**< Непрочитанные текущих участников, не постоянные ID. */
    uint32_t own_id; /**< Собственный ID; 0 до WELCOME. */
    uint32_t selected; /**< ID выбранного адресата либо 0. */
    int online; /**< -3 нет соединения, -2 подключение, -1 форма входа, 0 регистрация, 1 активен. */
    int contacts; /**< Фокус на списке; при узком окне список заменяет историю. */
    int help; /**< Открыта справка. */
    int scroll; /**< Число строк от конца истории либо от начала справки. */
    int saving; /**< Разрешено чтение при запуске/сохранение журнала; меняет контроллер. */
    char input[UI_INPUT_MAX]; /**< Редактор UTF-8, не связан с сетевым накопителем. */
    char notice[256]; /**< Последнее доверенное уведомление, подготовленное контроллером. */
    size_t cursor; /**< Байтовое смещение курсора в input. */
    UiMessage messages[UI_HISTORY_MAX]; /**< История по порядку, независимо от экрана. */
    unsigned message_count; /**< Число записей в messages. */
    ServerAddress servers[SERVER_BOOK_MAX]; /**< Копии сохранённых адресов; стандартный первый. */
    unsigned server_count; /**< Число адресов в меню. */
    unsigned server_focus; /**< Выделенный адрес. */
    int server_menu; /**< Открыто меню подключения. */
    int probe_state; /**< 0 неизвестно, 1 проверка, 2 отвечает WSCK, 3 нет ответа. */
    unsigned probe_capacity; /**< Настроенная ёмкость выделенного сервера. */
    unsigned probe_users; /**< Число участников по последней пробе выделенного адреса. */
    unsigned probe_latency; /**< RTT выделенного адреса, мс. */
    int probe_latency_valid; /**< Есть измерение RTT, а не предположение. */
    unsigned latency; /**< Последний RTT активного соединения, мс. */
    int latency_state; /**< 0 ещё нет замера, 1 измерен, -1 ответ просрочен. */
    FileInfo file; /**< Снимок независимого модуля передачи файла. */
    char chat_draft[UI_INPUT_MAX]; /**< Черновик при открытом меню серверов. */
    size_t draft_cursor; /**< Курсор сохранённого черновика. */
    char server_draft[UI_INPUT_MAX]; /**< Адрес, набранный в меню. */
    size_t server_cursor; /**< Курсор адреса. */
    unsigned completion_focus; /**< Индекс выбранной подсказки среди совпадений. */
    int completion_hidden; /**< Подсказки скрыты Esc/Tab до следующего редактирования. */
    int file_dialog; /**< 0 закрыт, 1 источник, 2 каталог приёма, 3 выбор default/каталога. */
    BrowserListing browser; /**< Собственный снимок каталога, полученный контроллером. */
    unsigned browser_focus; /**< 0 родительский каталог, иначе индекс записи + 1. */
    int browser_loading; /**< Ожидается фоновый снимок; выбор записей запрещён. */
    unsigned destination_choice; /**< 0 стандартная папка, 1 открыть файловую панель. */
    uint32_t file_dialog_peer; /**< Собеседник открытой панели, ID текущего сеанса. */
    char download_directory[BROWSER_PATH_MAX]; /**< Предложенная папка рядом с клиентом. */
    char file_draft[UI_INPUT_MAX]; /**< Черновик разговора при открытой файловой панели. */
    size_t file_cursor; /**< Курсор черновика. */
    char commands[UI_COMMAND_HISTORY_MAX][UI_INPUT_MAX]; /**< Команды от старой к новой, только память. */
    unsigned command_count; /**< Число сохранённых команд, максимум 20. */
    unsigned command_position; /**< 0 текущий черновик, 1 последняя команда, далее старые. */
    char command_draft[UI_INPUT_MAX]; /**< Ввод перед первым переходом вверх. */
    size_t command_cursor; /**< Точный курсор исходного черновика. */
} UiModel;

/** @brief Запомнить введённую slash-команду в ограниченной истории модели.
 * @param model Модель единственного владельца. @param text Проверяемая строка UTF-8 с NUL.
 * @return 1 добавлена, 0 обычный текст/неверная строка/повтор последней команды.
 * @note Копирует до UI_INPUT_MAX-1 байт; вытесняет старейшую запись. Завершает
 *       навигацию, не меняет редактор, ничего не пишет на диск и не выполняет команду. */
int Ui_RememberCommand(UiModel *model, const char *text);
/** @brief Перейти по истории с восстановлением исходного черновика и курсора.
 * @param model Модель владельца; input/cursor корректны по инварианту редактора.
 * @param direction -1 старее (вверх), 1 новее (вниз).
 * @return 1 редактор изменён, 0 граница/пустая история/неверное направление.
 * @note Записи не меняет; recalled-команда скрывает подсказки до редактирования.
 *       Вызов разрешает контроллер ввода только вне меню, панелей и списка участников. */
int Ui_RecallCommand(UiModel *model, int direction);
/** @brief Очистить команды и черновик навигации перед новым серверным сеансом.
 * @param model Модель одного владельца. @note Редактор не меняется, ресурсы не передаются;
 *       история команд не смешивается между подключениями и не входит в WSH1. */
void Ui_ClearCommands(UiModel *model);

/** @brief Число доступных сейчас подсказок команды.
 * @param model Неизменяемая модель основного потока.
 * @return Число совпадений; 0 в меню/справке/форме/аргументах либо после Esc.
 * @note Чистая функция, владение не меняется. */
unsigned Ui_Completions(const UiModel *model);
/** @brief Открыть/закрыть файловый интерфейс с отдельным черновиком.
 * @param model Модель владельца UI. @param mode 0 закрыть, 1 источник, 2 каталог, 3 выбор назначения.
 * @note Не обращается к диску/сети. Строки принадлежат модели. */
void Ui_FileDialog(UiModel *model, int mode);

/** @brief Выбрать перевод литерала без выделения памяти.
 * @param language Язык. @param en Английский текст. @param ru Русский текст.
 * @return Заимствованная строка en либо ru; аргументы должны быть живы при чтении результата. */
const char *Ui_Text(UiLanguage language, const char *en, const char *ru);
/** @brief Получить неизменяемую справку команд и клавиш на выбранном языке.
 * @param language Язык. @return Статическая строка UTF-8; освобождать нельзя. */
const char *Ui_Help(UiLanguage language);
/** @brief Оценить качество по RTT приложения: 3 до 100 мс, 2 до 250, 1 иначе.
 * @param latency Задержка мс. @param responding Есть свежий ответ.
 * @return Число видимых полос; не оценка полосы пропускания. Владения ресурсами нет. */
int Ui_Quality(unsigned latency, int responding);
/** @brief Переключить меню серверов, сохранив отдельные черновики редактора.
 * @param model Модель основного потока. @param open 1 открыть, 0 закрыть.
 * @note Не меняет соединение; строки остаются собственностью модели. */
void Ui_ServerMenu(UiModel *model, int open);
/** @brief Вставить проверенный UTF-8 по курсору без частичного изменения.
 * @param model Заимствованная модель основного потока. @param text Строка UTF-8 с NUL.
 * @return 1 при успехе, 0 при неверном тексте или переполнении. */
int Ui_Insert(UiModel *model, const char *text);
/** @brief Сдвинуть курсор либо удалить целый Unicode-символ.
 * @param model Модель основного потока. @param key -1 влево, 1 вправо, -2 Backspace, 2 Delete.
 * @pre input — корректный UTF-8, cursor на границе символа; владение не меняется. */
void Ui_Edit(UiModel *model, int key);
/** @brief Добавить копию сообщения в ограниченную историю, вытеснив старейшее.
 * @param model Модель основного потока. @param message Заимствованное корректное сообщение.
 * @return Адрес копии до следующего добавления/очистки; освобождать нельзя. */
UiMessage *Ui_Append(UiModel *model, const UiMessage *message);
/** @brief Найти участника по временному ID.
 * @param model Модель. @param id ID текущего сервера.
 * @return Заимствованный указатель до обновления списка либо NULL. */
const ProtocolParticipant *Ui_Peer(const UiModel *model, uint32_t id);
#endif
