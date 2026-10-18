/* fsu.h - file system calls on UTF-8 paths (Windows narrow APIs use the ANSI code page; the volume
 * holds Mac names such as "ColorSync™ Profiles", so Windows goes through the wide-character APIs). */
#ifndef FSU_H
#define FSU_H
#include <stdio.h>
#include <stdint.h>

FILE *u_fopen(const char *path, const char *mode);
int u_stat(const char *path, int64_t *size, int *isdir);     /* 0 = exists */
int u_mkdir(const char *path);
int u_rmdir(const char *path);
int u_remove(const char *path);
int u_rename(const char *from, const char *to);
int u_fullpath(const char *in, char *out, int max);           /* 0 = ok */
typedef struct UDir UDir;
UDir *u_opendir(const char *path);
const char *u_readdir(UDir *d);                                /* UTF-8 name, without . and .. */
void u_closedir(UDir *d);

#endif
