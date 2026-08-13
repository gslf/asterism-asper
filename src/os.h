/*
 * os.h — platform shim for libasper (threads, atomic replace, fs, time, rng).
 *
 * Implemented by os_posix.c (pthreads) and os_win32.c (Win32/SRW).
 * With ASPER_NO_THREADS defined, the locking primitives compile to no-ops
 * and os_thread_start returns ASPER_ERR_INVALID; os_common.c provides the
 * portable pieces shared by both backends.
 *
 * All paths are UTF-8; the Win32 backend converts to UTF-16 internally.
 */

#ifndef ASPER_OS_H
#define ASPER_OS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "asper.h" /* asper_err */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- opaque-ish primitive storage -------------------------------------- */

#if defined(ASPER_NO_THREADS)

typedef struct { int unused; } os_mutex;
typedef struct { int unused; } os_cond;
typedef struct { int unused; } os_rwlock;
typedef struct { int unused; } os_thread;

#elif defined(_WIN32)

/* Storage large enough for SRWLOCK / CONDITION_VARIABLE / HANDLE without
 * leaking <windows.h> into every translation unit. Checked with a static
 * assert in os_win32.c. */
typedef struct { void *h; } os_mutex;   /* SRWLOCK used exclusively */
typedef struct { void *h; } os_cond;    /* CONDITION_VARIABLE       */
typedef struct { void *h; } os_rwlock;  /* SRWLOCK                  */
typedef struct { void *h; } os_thread;  /* HANDLE                   */

#else /* POSIX */

#include <pthread.h>
typedef struct { pthread_mutex_t m; } os_mutex;
typedef struct { pthread_cond_t c; } os_cond;
typedef struct { pthread_rwlock_t l; } os_rwlock;
typedef struct { pthread_t t; int valid; } os_thread;

#endif

/* ---- threads ------------------------------------------------------------ */

asper_err os_thread_start(os_thread *t, void *(*fn)(void *), void *arg);
void      os_thread_join(os_thread *t);

void os_mutex_init(os_mutex *m);
void os_mutex_destroy(os_mutex *m);
void os_mutex_lock(os_mutex *m);
void os_mutex_unlock(os_mutex *m);

void os_cond_init(os_cond *c);
void os_cond_destroy(os_cond *c);
/* Wait with timeout in milliseconds; returns 1 if signaled, 0 on timeout. */
int  os_cond_timedwait(os_cond *c, os_mutex *m, int64_t timeout_ms);
void os_cond_wait(os_cond *c, os_mutex *m);
void os_cond_signal(os_cond *c);
void os_cond_broadcast(os_cond *c);

void os_rwlock_init(os_rwlock *l);
void os_rwlock_destroy(os_rwlock *l);
void os_rwlock_rdlock(os_rwlock *l);
void os_rwlock_rdunlock(os_rwlock *l);
void os_rwlock_wrlock(os_rwlock *l);
void os_rwlock_wrunlock(os_rwlock *l);

/* ---- filesystem --------------------------------------------------------- */

/* Atomically replace dst with src (rename(2) / ReplaceFileW+MoveFileExW).
 * src is consumed on success. */
asper_err os_file_replace(const char *src, const char *dst);
/* fsync/_commit an open stream. */
asper_err os_fsync(FILE *f);
/* Create directory and any missing parents. Existing dir is OK. */
asper_err os_mkdir_p(const char *path);
int       os_file_exists(const char *path);   /* 1 = yes */
/* Read whole file. *out is NUL-terminated (asper internal: free with free()).
 * out_len may be NULL. */
asper_err os_read_file(const char *path, char **out, size_t *out_len);
/* Write whole buffer (truncate). Not atomic — pair with os_file_replace for
 * atomic rewrites. */
asper_err os_write_file(const char *path, const void *data, size_t len);
asper_err os_remove_file(const char *path);
/* Truncate file to size bytes (journal torn-tail repair). */
asper_err os_truncate(const char *path, uint64_t size);
/* File size in bytes; ASPER_ERR_NOT_FOUND if missing. */
asper_err os_file_size(const char *path, uint64_t *out);
/* Rename within the same directory tree, replacing target if present
 * (log rotation). */
asper_err os_rename(const char *src, const char *dst);
/* List regular file names (no dirs) in path, malloc'd array of malloc'd
 * names, unsorted; caller frees each + array. Missing dir => 0 entries. */
asper_err os_list_dir(const char *path, char ***out_names, size_t *out_n);
/* Open a FILE* with UTF-8 path (fopen wrapper; _wfopen on Windows). */
FILE     *os_fopen(const char *path, const char *mode);

/* ---- misc --------------------------------------------------------------- */

int64_t os_now_unix(void);                 /* wall clock, unix seconds UTC */
int64_t os_monotonic_ms(void);             /* monotonic milliseconds      */
void    os_random_bytes(void *buf, size_t n); /* CSPRNG or best effort    */
int     os_hardware_threads(void);         /* >= 1 */
/* Path join with '/' (also fine on Windows APIs used here). Returns
 * malloc'd string. */
char   *os_path_join(const char *a, const char *b);

#ifdef __cplusplus
}
#endif

#endif /* ASPER_OS_H */
