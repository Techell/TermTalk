#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "file_browser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>

struct FileBrowser {
    HANDLE thread;
    volatile LONG done, cancel;
    wchar_t path[1024];
    int drives;
    BrowserListing listing;
};
static int Wide(const char *input, wchar_t *output, int capacity)
{ return MultiByteToWideChar(CP_UTF8, 0x8, input, -1, output, capacity) != 0; }
static int Utf8(const wchar_t *input, char *output, size_t capacity)
{ return capacity <= INT_MAX && WideCharToMultiByte(CP_UTF8, 0x80, input, -1, output, (int)capacity, NULL, NULL) != 0; }
static int Compare(const void *a, const void *b)
{
    const BrowserEntry *x = a, *y = b;
    wchar_t left[1024], right[1024];
    if (x->directory != y->directory) return y->directory - x->directory;
    Wide(x->name, left, 1024); Wide(y->name, right, 1024);
    return lstrcmpiW(left, right);
}
static DWORD WINAPI Enumerate(LPVOID context)
{
    typedef BOOL (WINAPI *ThreadErrorMode)(DWORD, LPDWORD);
    FileBrowser *b = context;
    BrowserListing *l = &b->listing;
    WIN32_FIND_DATAW item;
    HANDLE search;
    wchar_t pattern[1030];
    FARPROC address = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadErrorMode");
    ThreadErrorMode error_mode = NULL; DWORD previous_mode = 0; int mode_changed;
    memcpy(&error_mode, &address, sizeof(error_mode));
    /* Пустой съёмный диск должен вернуть ошибку в панель, а не показать
       системное модальное окно. Меняем только режим собственного работника. */
    mode_changed = error_mode && error_mode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous_mode);
    memset(l, 0, sizeof(*l));
    if (b->drives) {
        DWORD drives = GetLogicalDrives(); unsigned i;
        if (!drives) l->error = GetLastError();
        for (i = 0; i < 26; ++i) if (drives & (1u << i)) {
            BrowserEntry *e = &l->entries[l->count++];
            snprintf(e->name, sizeof(e->name), "%c:\\", 'A' + i); e->directory = 1;
        }
        goto done;
    }
    if (!Utf8(b->path, l->directory, sizeof(l->directory))) { l->error = GetLastError(); goto done; }
    wcscpy(pattern, b->path); if (pattern[wcslen(pattern) - 1] != L'\\') wcscat(pattern, L"\\"); wcscat(pattern, L"*");
    search = FindFirstFileW(pattern, &item);
    if (search == INVALID_HANDLE_VALUE) {
        l->error = GetLastError(); if (l->error == ERROR_FILE_NOT_FOUND) l->error = 0;
        goto done;
    }
    do {
        BrowserEntry *e;
        if (InterlockedCompareExchange(&b->cancel, 0, 0)) break;
        if (!wcscmp(item.cFileName, L".") || !wcscmp(item.cFileName, L"..")) continue;
        if (l->count == BROWSER_ENTRIES_MAX) { l->truncated = 1; break; }
        e = &l->entries[l->count];
        if (!Utf8(item.cFileName, e->name, sizeof(e->name))) continue;
        e->directory = !!(item.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
        e->size = e->directory ? 0 : ((uint64_t)item.nFileSizeHigh << 32) | item.nFileSizeLow;
        ++l->count;
    } while (FindNextFileW(search, &item));
    if (!l->truncated && !InterlockedCompareExchange(&b->cancel, 0, 0)) {
        DWORD error = GetLastError(); if (error != ERROR_NO_MORE_FILES) l->error = error;
    }
    FindClose(search); qsort(l->entries, l->count, sizeof(*l->entries), Compare);
done:
    if (mode_changed) error_mode(previous_mode, NULL);
    /* Только после закрытия поиска публикуется полностью собственный снимок. */
    InterlockedExchange(&b->done, 1); return 0;
}
FileBrowser *Browser_Create(void) { return calloc(1, sizeof(FileBrowser)); }
int Browser_Request(FileBrowser *b, const char *path)
{
    wchar_t input[1024]; DWORD n;
    if (b->thread) return 0;
    b->drives = path == NULL;
    if (path) {
        if (!*path || !Wide(path, input, 1024)) return -1;
        n = GetFullPathNameW(input, 1020, b->path, NULL); if (!n || n >= 1020) return -1;
    }
    InterlockedExchange(&b->cancel, 0); InterlockedExchange(&b->done, 0);
    b->thread = CreateThread(NULL, 0, Enumerate, b, 0, NULL); return b->thread ? 1 : -1;
}
int Browser_Read(FileBrowser *b, BrowserListing *listing)
{
    if (!b->thread || !InterlockedCompareExchange(&b->done, 0, 0)) return 0;
    WaitForSingleObject(b->thread, INFINITE); CloseHandle(b->thread); b->thread = NULL;
    *listing = b->listing; return 1;
}
void Browser_Cancel(FileBrowser *b) { InterlockedExchange(&b->cancel, 1); }
void Browser_Destroy(FileBrowser *b)
{
    if (!b) return;
    Browser_Cancel(b); if (b->thread) { WaitForSingleObject(b->thread, INFINITE); CloseHandle(b->thread); }
    free(b);
}
int Browser_ClientDirectory(int downloads, char *output, size_t capacity)
{
    wchar_t path[1024], *last;
    DWORD n = GetModuleFileNameW(NULL, path, 1000);
    if (!n || n >= 1000 || !(last = wcsrchr(path, L'\\'))) return 0;
    last[1] = 0; if (downloads) wcscat(path, L"downloads");
    return Utf8(path, output, capacity);
}
int Browser_Parent(const char *directory, char *output, size_t capacity)
{
    wchar_t input[1024], full[1024], *last;
    DWORD n;
    if (!*directory) { if (capacity) *output = 0; return capacity != 0; }
    if (!Wide(directory, input, 1024)) return 0;
    n = GetFullPathNameW(input, 1024, full, NULL); if (!n || n >= 1024) return 0;
    n = (DWORD)wcslen(full);
    while (n > 3 && full[n - 1] == L'\\') full[--n] = 0;
    if (full[0] == L'\\' && full[1] == L'\\') {
        wchar_t *server_end = wcschr(full + 2, L'\\');
        if (!server_end || !wcschr(server_end + 1, L'\\')) { if (capacity) *output = 0; return capacity != 0; }
    }
    if (n <= 3) { if (capacity) *output = 0; return capacity != 0; }
    last = wcsrchr(full, L'\\'); if (!last) return 0;
    if (last == full + 2) last[1] = 0; else *last = 0;
    return Utf8(full, output, capacity);
}
int Browser_EntryPath(const BrowserListing *l, unsigned index, char *output, size_t capacity)
{
    size_t n;
    if (index >= l->count) return 0;
    n = strlen(l->directory);
    if (n + strlen(l->entries[index].name) + 2 > capacity) return 0;
    strcpy(output, l->directory);
    if (n && output[n - 1] != '\\') strcat(output, "\\");
    strcat(output, l->entries[index].name); return 1;
}
int Browser_ReceivePath(const char *directory, const char *name, int create_directory, char *output, size_t capacity)
{
    wchar_t path[1024], filename[256], candidate[1200], *extension;
    size_t n, i; unsigned attempt;
    DWORD attributes;
    if (!Wide(directory, path, 1024) || !Wide(name, filename, 256) || !*filename) return 0;
    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES && create_directory && CreateDirectoryW(path, NULL)) attributes = FILE_ATTRIBUTE_DIRECTORY;
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    n = wcslen(path); if (n > 900) return 0;
    if (n && path[n - 1] != L'\\') wcscat(path, L"\\");
    for (i = 0; filename[i]; ++i) if (filename[i] < 32 || wcschr(L"<>:\"/\\|?*", filename[i])) filename[i] = L'_';
    n = wcslen(filename); while (n && (filename[n - 1] == L'.' || filename[n - 1] == L' ')) filename[--n] = 0;
    if (!n) wcscpy(filename, L"file");
    /* Префикс для DOS device names, включая имя перед расширением. */
    { wchar_t stem[256], prefix[4], *dot;
      wcscpy(stem, filename); dot = wcschr(stem, L'.'); if (dot) *dot = 0;
      n = wcslen(stem); while (n && stem[n - 1] == L' ') stem[--n] = 0;
      wcsncpy(prefix, stem, 3); prefix[3] = 0;
      if (!lstrcmpiW(stem, L"CON") || !lstrcmpiW(stem, L"PRN") || !lstrcmpiW(stem, L"AUX") || !lstrcmpiW(stem, L"NUL")
          || (wcslen(stem) == 4 && (!lstrcmpiW(prefix, L"COM") || !lstrcmpiW(prefix, L"LPT"))
              && ((stem[3] >= L'1' && stem[3] <= L'9') || stem[3] == 0xb9 || stem[3] == 0xb2 || stem[3] == 0xb3))) {
          if (wcslen(filename) >= 254) return 0;
          memmove(filename + 1, filename, (wcslen(filename) + 1) * sizeof(wchar_t)); filename[0] = L'_';
      } }
    extension = wcsrchr(filename, L'.'); if (extension == filename) extension = NULL;
    for (attempt = 0; attempt < 1000; ++attempt) {
        wchar_t suffix[20], ext[256]; char narrow[20];
        wcscpy(candidate, path);
        if (!attempt) wcscat(candidate, filename);
        else {
            wcscpy(ext, extension ? extension : L""); if (extension) *extension = 0;
            wcscat(candidate, filename); snprintf(narrow, sizeof(narrow), " (%u)", attempt); Wide(narrow, suffix, 20);
            wcscat(candidate, suffix); wcscat(candidate, ext); if (extension) *extension = L'.';
        }
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) {
            DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) return 0;
            return Utf8(candidate, output, capacity);
        }
    }
    return 0;
}
