#ifndef WINSOCK_COMMAND_CATALOG_H
#define WINSOCK_COMMAND_CATALOG_H
/** @file command_catalog.h @brief Чистый каталог команд для подсказок, без консоли и сети. */
#include <stddef.h>
/** Статическая запись; все строки принадлежат модулю. */
typedef struct {
    const char *name; /**< Команда со слешем. */
    const char *arguments; /**< Краткая схема аргументов, пусто для команды без них. */
    const char *english; /**< Английское описание. */
    const char *russian; /**< Русское описание. */
} CommandHint;
/** @brief Найти команду по префиксу первого токена.
 * @param input UTF-8 строка с NUL; пробел/перевод строки завершает подбор.
 * @param index Индекс совпадения, начиная с 0.
 * @return Заимствованная статическая запись либо NULL. Аргументы не удерживаются. */
const CommandHint *Command_Match(const char *input, unsigned index);
/** @brief Посчитать совпадения первого токена со слешем.
 * @param input Строка с NUL. @return Число записей; 0 для обычного текста/аргументов.
 * @note Чистая функция без изменения владения. */
unsigned Command_Count(const char *input);
#endif
