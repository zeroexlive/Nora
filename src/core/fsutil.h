/* Nora - small portable file system helpers: paths, file times, whole-file reads.
 * MIT License. */
#ifndef NORA_FSUTIL_H
#define NORA_FSUTIL_H

#include <stddef.h>
#include <stdint.h>

/* Reads a whole file (NUL terminated). Returns NULL on failure; *len may be NULL. */
char *nora_read_file(const char *path, size_t *len);
/* Writes a whole file through a temporary file and a rename. 0 on success. */
int nora_write_file_atomic(const char *path, const void *data, size_t len);

/* Path helpers. Separators '/' and '\\' are both accepted; '/' is written. */
void nora_path_dir(const char *path, char *out, size_t cap);           /* "." when there is no directory */
const char *nora_path_base(const char *path);
const char *nora_path_ext(const char *path);                             /* ".obj" or "" */
int nora_path_is_abs(const char *path);
void nora_path_join(const char *dir, const char *rel, char *out, size_t cap);  /* rel if absolute */
/* Absolute and normalised (".", ".." and repeated separators removed). */
void nora_path_abs(const char *path, char *out, size_t cap);
/* path relative to dir (both made absolute first); falls back to the absolute path
 * when they are on different roots/drives. */
void nora_path_rel(const char *path, const char *dir, char *out, size_t cap);

/* File state for change detection: modification time in nanoseconds and size.
 * Returns 0 if the file exists. */
typedef struct nora_fstat { int64_t mtime_ns; int64_t size; int exists; } nora_fstat;
int nora_file_stat(const char *path, nora_fstat *st);

#endif
