#ifndef TERMTALK_BRANDING_H
#define TERMTALK_BRANDING_H
/** @file branding.h
 * @brief Общие подписи TermTalk; не меняют WSCK или каталог старых профилей.
 * @note Строковые литералы статические, не требуют освобождения; ASCII-баннер
 *       безопасен для перенаправленных потоков и старых кодовых страниц сервера.
 */
/** @brief Короткое имя приложения. */
#define TERMTALK_NAME "TermTalk"
/** @brief Текстовый логотип для запуска клиента/сервера без экранного адаптера. */
#define TERMTALK_BANNER \
    "+---+  TermTalk\n" \
    "| > |  Messages. Files. Terminal.\n" \
    "+---+\n"
#endif
