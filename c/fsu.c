/* fsu.c - file system calls on UTF-8 paths (Windows: wide-character APIs; POSIX: as is). */
#include "fsu.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#include <direct.h>
#include <io.h>
static wchar_t *W16(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = malloc((size_t)(n > 0 ? n : 1) * sizeof(wchar_t));
    if (n <= 0 || !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) w[0] = 0;
    return w;
}
static void U8(const wchar_t *w, char *out, int max) {
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, max, NULL, NULL)) out[0] = 0;
}
FILE *u_fopen(const char *path, const char *mode) {
    wchar_t *p = W16(path), *m = W16(mode); FILE *f = _wfopen(p, m); free(p); free(m); return f;
}
int u_stat(const char *path, int64_t *size, int *isdir) {
    wchar_t *p = W16(path); WIN32_FILE_ATTRIBUTE_DATA a; BOOL ok = GetFileAttributesExW(p, GetFileExInfoStandard, &a); free(p);
    if (!ok) return -1;
    if (size) *size = ((int64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    if (isdir) *isdir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    return 0;
}
int u_mkdir(const char *path) { wchar_t *p = W16(path); int r = _wmkdir(p); free(p); return r; }
int u_rmdir(const char *path) { wchar_t *p = W16(path); int r = _wrmdir(p); free(p); return r; }
int u_remove(const char *path) { wchar_t *p = W16(path); int r = _wremove(p); free(p); return r; }
int u_rename(const char *a, const char *b) { wchar_t *p = W16(a), *q = W16(b); int r = _wrename(p, q); free(p); free(q); return r; }
int u_fullpath(const char *in, char *out, int max) {
    wchar_t *p = W16(in), buf[4096]; DWORD n = GetFullPathNameW(p, 4096, buf, NULL); free(p);
    if (!n || n >= 4096) return -1;
    U8(buf, out, max); return 0;
}
struct UDir { HANDLE h; WIN32_FIND_DATAW fd; int first; char name[1024]; };
UDir *u_opendir(const char *path) {
    size_t n = strlen(path); char *pat = malloc(n + 3); memcpy(pat, path, n); memcpy(pat + n, "/*", 3);
    wchar_t *w = W16(pat); free(pat);
    UDir *d = calloc(1, sizeof *d); d->h = FindFirstFileW(w, &d->fd); free(w);
    if (d->h == INVALID_HANDLE_VALUE) { free(d); return NULL; }
    d->first = 1; return d;
}
const char *u_readdir(UDir *d) {
    for (;;) {
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return NULL;
        d->first = 0;
        if (!wcscmp(d->fd.cFileName, L".") || !wcscmp(d->fd.cFileName, L"..")) continue;
        U8(d->fd.cFileName, d->name, sizeof d->name); return d->name;
    }
}
void u_closedir(UDir *d) { if (d) { FindClose(d->h); free(d); } }
#else
#include <dirent.h>
#include <unistd.h>
#include <limits.h>
#include <stdio.h>
FILE *u_fopen(const char *path, const char *mode) { return fopen(path, mode); }
int u_stat(const char *path, int64_t *size, int *isdir) {
    struct stat st; if (stat(path, &st)) return -1;
    if (size) *size = st.st_size;
    if (isdir) *isdir = S_ISDIR(st.st_mode);
    return 0;
}
int u_mkdir(const char *path) { return mkdir(path, 0777); }
int u_rmdir(const char *path) { return rmdir(path); }
int u_remove(const char *path) { return remove(path); }
int u_rename(const char *a, const char *b) { return rename(a, b); }
int u_fullpath(const char *in, char *out, int max) {
    char buf[PATH_MAX]; if (!realpath(in, buf)) return -1;
    snprintf(out, (size_t)max, "%s", buf); return 0;
}
struct UDir { DIR *d; };
UDir *u_opendir(const char *path) { DIR *x = opendir(path); if (!x) return NULL; UDir *d = calloc(1, sizeof *d); d->d = x; return d; }
const char *u_readdir(UDir *d) {
    struct dirent *e;
    while ((e = readdir(d->d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; return e->d_name; }
    return NULL;
}
void u_closedir(UDir *d) { if (d) { closedir(d->d); free(d); } }
#endif
