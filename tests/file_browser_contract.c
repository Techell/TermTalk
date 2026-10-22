/** @file file_browser_contract.c @brief Реальная файловая система: Unicode, снимки, коллизии и отмена. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "file_browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "line %d: %s Win32=%lu\n", __LINE__, #c, (unsigned long)GetLastError()); exit(1); } } while (0)
static void Touch(const char *path)
{
    wchar_t wide[1024]; HANDLE file;
    CHECK(MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024));
    file = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL); CHECK(file != INVALID_HANDLE_VALUE); CloseHandle(file);
}
static void Delete(const char *path)
{
    wchar_t wide[1024]; CHECK(MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024)); CHECK(DeleteFileW(wide));
}
static void Read(FileBrowser *browser, BrowserListing *listing)
{
    unsigned i; for (i = 0; i < 1000 && !Browser_Read(browser, listing); ++i) Sleep(2);
    CHECK(i < 1000);
}
int main(void)
{
    char root[BROWSER_PATH_MAX], path[BROWSER_PATH_MAX], next[BROWSER_PATH_MAX], parent[BROWSER_PATH_MAX];
    wchar_t wide[1024];
    FileBrowser *browser = Browser_Create(); BrowserListing *listing = calloc(1, sizeof(*listing));
    unsigned i;
    CHECK(browser && listing); CHECK(Browser_ClientDirectory(0, root, sizeof(root)));
    snprintf(path, sizeof(path), "%sбраузер-%lu", root, (unsigned long)GetCurrentProcessId()); strcpy(root, path);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, root, -1, wide, 1024)); CHECK(CreateDirectoryW(wide, NULL));
    snprintf(path, sizeof(path), "%s\\данные.bin", root); Touch(path);
    snprintf(next, sizeof(next), "%s\\папка", root); CHECK(MultiByteToWideChar(CP_UTF8, 0, next, -1, wide, 1024)); CHECK(CreateDirectoryW(wide, NULL));
    CHECK(Browser_Request(browser, root) == 1); CHECK(Browser_Request(browser, root) == 0); Read(browser, listing);
    CHECK(!listing->error && listing->count == 2 && listing->entries[0].directory && !listing->entries[1].directory);
    CHECK(!strcmp(listing->entries[0].name, "папка") && !strcmp(listing->entries[1].name, "данные.bin"));
    CHECK(Browser_EntryPath(listing, 1, next, sizeof(next)) && !strcmp(path, next));
    CHECK(Browser_Parent(path, parent, sizeof(parent)) && !strcmp(parent, root));
    CHECK(Browser_Parent("C:\\", parent, sizeof(parent)) && !*parent);
    CHECK(Browser_Parent("\\\\host\\share\\", parent, sizeof(parent)) && !*parent);
    CHECK(!Browser_EntryPath(listing, 3, next, sizeof(next)));
    CHECK(Browser_ReceivePath(root, "данные.bin", 0, next, sizeof(next)) && strstr(next, "данные (1).bin")); Touch(next);
    CHECK(Browser_ReceivePath(root, "данные.bin", 0, parent, sizeof(parent)) && strstr(parent, "данные (2).bin"));
    CHECK(Browser_ReceivePath(root, "CON.txt", 0, parent, sizeof(parent)) && strstr(parent, "_CON.txt"));
    CHECK(Browser_ReceivePath(root, "COM¹.txt", 0, parent, sizeof(parent)) && strstr(parent, "_COM¹.txt"));
    CHECK(Browser_ReceivePath(root, "a?.bin", 0, parent, sizeof(parent)) && strstr(parent, "a_.bin"));
    CHECK(Browser_ReceivePath(root, "..", 0, parent, sizeof(parent)) && strstr(parent, "\\file"));
    CHECK(!Browser_ReceivePath(root, "a", 0, parent, 2));
    Delete(path); Delete(next);
    snprintf(path, sizeof(path), "%s\\папка", root); CHECK(MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, 1024)); CHECK(RemoveDirectoryW(wide));
    for (i = 0; i < BROWSER_ENTRIES_MAX + 1; ++i) { snprintf(path, sizeof(path), "%s\\%04u.bin", root, i); Touch(path); }
    CHECK(Browser_Request(browser, root) == 1); Read(browser, listing); CHECK(!listing->error && listing->count == BROWSER_ENTRIES_MAX && listing->truncated);
    for (i = 0; i < BROWSER_ENTRIES_MAX + 1; ++i) { snprintf(path, sizeof(path), "%s\\%04u.bin", root, i); Delete(path); }
    CHECK(Browser_Request(browser, root) == 1); Browser_Cancel(browser); Read(browser, listing);
    CHECK(Browser_Request(browser, NULL) == 1); Read(browser, listing); CHECK(!listing->error && listing->count && !*listing->directory);
    CHECK(Browser_Request(browser, "") == -1);
    CHECK(MultiByteToWideChar(CP_UTF8, 0, root, -1, wide, 1024)); CHECK(RemoveDirectoryW(wide));
    CHECK(Browser_Request(browser, root) == 1); Read(browser, listing); CHECK(listing->error);
    Browser_Destroy(browser); free(listing);
    puts("PASS: async Unicode directory listing, directories first, parent/drives, bounded results, cancellation, safe unique receive names"); return 0;
}
