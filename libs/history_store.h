#ifndef WINSOCK_HISTORY_STORE_H
#define WINSOCK_HISTORY_STORE_H
/** @file history_store.h @brief Ограниченное локальное хранилище; не зависит от консольного адаптера. */
#include "ui_model.h"
#include <wchar.h>
/** @brief Путь профиля принадлежит объекту; файл содержит открытый текст, не ключи. */
typedef struct {
    wchar_t directory[1024]; /**< Каталог UTF-16 с NUL; создаётся только при сохранении. */
    wchar_t path[1200]; /**< Полный путь файла профиля, без сырого никнейма. */
} HistoryStore;
/** @brief Подготовить путь без создания файлов.
 * @param store Выходной объект вызывающего. @param model Модель с endpoint и nickname.
 * @param directory Каталог UTF-16 либо NULL для LOCALAPPDATA/WinSock/history.
 * @return 1 при успехе, 0 при невозможном/слишком длинном пути. Владение строками не передаётся. */
int History_Init(HistoryStore *store, const UiModel *model, const wchar_t *directory);
/** @brief Загрузить проверенный WSH1 целиком; повреждение не меняет модель.
 * @param store Подготовленное хранилище. @param model Модель основного потока, получает копии сообщений.
 * @return 1 успех (также отсутствие файла), 0 ошибка формата/чтения/ресурсов.
 * @note Не создаёт каталогов; не вызывать при отключённой истории. */
int History_Load(const HistoryStore *store, UiModel *model);
/** @brief Атомарно заменить журнал ограниченной копией истории.
 * @param store Подготовленное хранилище. @param model Заимствованная неизменяемая модель.
 * @return 1 успех, 0 ошибка; старый файл сохраняется до успешной замены.
 * @note Вызывается владельцем модели, после остановки сети либо явной командой. */
int History_Save(const HistoryStore *store, const UiModel *model);
/** @brief Удалить только файл выбранного профиля, не каталог.
 * @param store Подготовленное хранилище. @return 1 удалён/не существовал, 0 ошибка WinAPI.
 * @note Не очищает модель; вызывающий очищает её только при успехе. */
int History_Clear(const HistoryStore *store);
#endif
