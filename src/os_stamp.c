/* Observe the open file, including changes hidden by a stdio read buffer. */
#ifdef _WIN32
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "os.h"
#include <string.h>

asper_err os_stream_stamp(FILE *stream, os_file_stamp *stamp) {
  memset(stamp, 0, sizeof *stamp);
#ifdef _WIN32
  HANDLE handle = (HANDLE)_get_osfhandle(_fileno(stream));
  BY_HANDLE_FILE_INFORMATION info;
  FILE_BASIC_INFO basic;
  if (!GetFileInformationByHandle(handle, &info) ||
      !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, (DWORD)sizeof basic)) return ASPER_ERR_IO;
  stamp->size = ((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow;
  stamp->links = info.nNumberOfLinks;
  stamp->modified[0] = (uint64_t)basic.LastWriteTime.QuadPart;
  stamp->changed[0] = (uint64_t)basic.ChangeTime.QuadPart;
#else
  struct stat st;
  if (fstat(fileno(stream), &st)) return ASPER_ERR_IO;
  if (!S_ISREG(st.st_mode) || st.st_size < 0) return ASPER_ERR_INVALID;
  stamp->size = (uint64_t)st.st_size; stamp->links = (uint64_t)st.st_nlink;
#ifdef __APPLE__
  stamp->modified[0] = (uint64_t)st.st_mtimespec.tv_sec;
  stamp->modified[1] = (uint64_t)st.st_mtimespec.tv_nsec;
  stamp->changed[0] = (uint64_t)st.st_ctimespec.tv_sec;
  stamp->changed[1] = (uint64_t)st.st_ctimespec.tv_nsec;
#else
  stamp->modified[0] = (uint64_t)st.st_mtim.tv_sec;
  stamp->modified[1] = (uint64_t)st.st_mtim.tv_nsec;
  stamp->changed[0] = (uint64_t)st.st_ctim.tv_sec;
  stamp->changed[1] = (uint64_t)st.st_ctim.tv_nsec;
#endif
#endif
  return ASPER_OK;
}
