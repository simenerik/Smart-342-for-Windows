/* prefscopy.h - Scanner(Flash Disk) --> Mac(Smart342.Prefs) copy (see prefscopy.c) */
#ifndef PREFSCOPY_H
#define PREFSCOPY_H
#include "mac.h"
/* 1 = the flash file as the scanner sends it is complete and consistent (length, checksum, resource map) */
int flash_file_check(const uint8_t *f, size_t n, char *msg, int msgmax);
/* the scanner number in a prefs resource fork (STR# 300, string 1, up to the first blank) */
int prefs_serial(const uint8_t *fork, size_t n, char *out, int outmax);
/* the new prefs resource fork in *out (free with bytes_free); flash_serial / prefs_serial_out: the two numbers.
   Returns the number of resources copied (0 = refused, msg says why). */
int prefs_copy_from_flash(const uint8_t *flash, size_t nflash, const uint8_t *prefs, size_t nprefs, int lw_butting,
                          Bytes *out, char *flash_serial, char *prefs_serial_out, int sermax, char *msg, int msgmax);
/* 0.3.1: replace (or add) n resources of `type` (attributes 0); returns n, 0 = refused */
int prefs_put_resources(const uint8_t *prefs, size_t nprefs, const char *type, const int *ids, const char *const *names,
                        const uint8_t *const *data, const size_t *ndata, int n, Bytes *out, char *msg, int msgmax);
/* 0.3.1: a copy of one resource's data; its length, -1 = none */
long prefs_get_resource(const uint8_t *prefs, size_t nprefs, const char *type, int id, uint8_t *out, size_t outmax);
#endif
