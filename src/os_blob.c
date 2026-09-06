/* Read regular files without traversing aliases. The returned stream owns its handle. */
#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#define _FILE_OFFSET_BITS 64
#else
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <io.h>
#include <wchar.h>
#endif
#include "os.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>

asper_err os_directory_canonical(const char *path, char **out) {
  *out = NULL;
  char *resolved = realpath(path, NULL);
  if (!resolved) return errno == ENOENT ? ASPER_ERR_NOT_FOUND : ASPER_ERR_IO;
  struct stat st;
  if (stat(resolved, &st) || !S_ISDIR(st.st_mode)) { free(resolved); return ASPER_ERR_INVALID; }
  *out = resolved; return ASPER_OK;
}

asper_err os_blob_open(const char *path, FILE **out, uint64_t *size) {
  *out = NULL; *size = 0;
  if (!path || !*path) return ASPER_ERR_INVALID;
  char *parts = strdup(path), *state = NULL;
  if (!parts) return ASPER_ERR_NOMEM;
  int fd = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  asper_err e = fd < 0 ? ASPER_ERR_IO : ASPER_OK;
  char *part = strtok_r(parts, "/", &state);
  while (e == ASPER_OK && part) {
    char *next = strtok_r(NULL, "/", &state);
    int opened = openat(fd, part, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK | (next ? O_DIRECTORY : 0));
    if (opened < 0) e = errno == ENOENT ? ASPER_ERR_NOT_FOUND : ASPER_ERR_INVALID;
    else { close(fd); fd = opened; }
    part = next;
  }
  free(parts);
  if (e == ASPER_OK) {
    struct stat st;
    if (fstat(fd, &st)) e = ASPER_ERR_IO;
    else if (!S_ISREG(st.st_mode) || st.st_size < 0) e = ASPER_ERR_INVALID;
    else {
      *size = (uint64_t)st.st_size;
      *out = fdopen(fd, "rb");
      if (*out) fd = -1;
      else e = ASPER_ERR_IO;
    }
  }
  if (fd >= 0) close(fd);
  if (e != ASPER_OK) *size = 0;
  return e;
}
#else
static wchar_t *dos_name(wchar_t *path) {
  if (!wcsncmp(path, L"\\\\?\\UNC\\", 8)) { path[6] = L'\\'; return path + 6; }
  return !wcsncmp(path, L"\\\\?\\", 4) ? path + 4 : path;
}
asper_err os_directory_canonical(const char *path, char **out) {
  *out = NULL;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (!n) return ASPER_ERR_INVALID;
  wchar_t *wide = malloc((size_t)n * sizeof *wide), *final = NULL;
  if (!wide) return ASPER_ERR_NOMEM;
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) { free(wide); return ASPER_ERR_INVALID; }
  HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  free(wide);
  if (handle == INVALID_HANDLE_VALUE) return ASPER_ERR_IO;
  BY_HANDLE_FILE_INFORMATION info;
  asper_err e = ASPER_OK;
  if (!GetFileInformationByHandle(handle, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) e = ASPER_ERR_INVALID;
  DWORD size = e == ASPER_OK ? GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED) : 0;
  if (e == ASPER_OK && !size) e = ASPER_ERR_IO;
  if (e == ASPER_OK) {
    final = malloc((size_t)size * sizeof *final);
    if (!final) e = ASPER_ERR_NOMEM;
    else {
      DWORD got = GetFinalPathNameByHandleW(handle, final, size, FILE_NAME_NORMALIZED);
      if (!got || got >= size) e = ASPER_ERR_IO;
      else {
        const wchar_t *normalized = dos_name(final);
        n = WideCharToMultiByte(CP_UTF8, 0, normalized, -1, NULL, 0, NULL, NULL);
        if (!n) e = ASPER_ERR_IO;
        else {
          *out = malloc((size_t)n);
          if (!*out) e = ASPER_ERR_NOMEM;
          else if (!WideCharToMultiByte(CP_UTF8, 0, normalized, -1, *out, n, NULL, NULL)) e = ASPER_ERR_IO;
        }
      }
    }
  }
  if (e != ASPER_OK) { free(*out); *out = NULL; }
  free(final); CloseHandle(handle); return e;
}

asper_err os_blob_open(const char *path, FILE **out, uint64_t *size) {
  *out = NULL; *size = 0;
  if (!path || !*path) return ASPER_ERR_INVALID;
  int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (!n) return ASPER_ERR_INVALID;
  wchar_t *wide = malloc((size_t)n * sizeof *wide), *expected = NULL, *actual = NULL;
  if (!wide) return ASPER_ERR_NOMEM;
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, n)) { free(wide); return ASPER_ERR_INVALID; }
  HANDLE handle = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
      NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  asper_err e = ASPER_OK;
  if (handle == INVALID_HANDLE_VALUE) {
    DWORD error = GetLastError(); free(wide);
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ASPER_ERR_NOT_FOUND : ASPER_ERR_IO;
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (GetFileType(handle) != FILE_TYPE_DISK || !GetFileInformationByHandle(handle, &info) ||
      (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) e = ASPER_ERR_INVALID;
  if (e == ASPER_OK) {
    DWORD a = GetFullPathNameW(wide, 0, NULL, NULL), b = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED);
    if (!a || !b) e = ASPER_ERR_IO;
    else {
      expected = malloc((size_t)a * sizeof *expected); actual = malloc((size_t)b * sizeof *actual);
      if (!expected || !actual) e = ASPER_ERR_NOMEM;
      else {
        DWORD x = GetFullPathNameW(wide, a, expected, NULL), y = GetFinalPathNameByHandleW(handle, actual, b, FILE_NAME_NORMALIZED);
        if (!x || x >= a || !y || y >= b) e = ASPER_ERR_IO;
        else if (_wcsicmp(dos_name(expected), dos_name(actual))) e = ASPER_ERR_INVALID;
      }
    }
  }
  if (e == ASPER_OK) {
    int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) e = ASPER_ERR_IO;
    else {
      handle = INVALID_HANDLE_VALUE;
      *out = _fdopen(fd, "rb");
      if (!*out) { _close(fd); e = ASPER_ERR_IO; }
      else *size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    }
  }
  if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  free(wide); free(expected); free(actual); return e;
}
#endif
