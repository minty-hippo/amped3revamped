// Linux build of AMD's FidelityFX SDK (FSR 3.1 upscaler, Vulkan): stand-ins
// for the Microsoft C runtime functions and keywords the SDK's sources use.
// Force-included into every source file (-include).
#pragma once

#ifndef _WIN32

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define __declspec(x) __attribute__((visibility("default")))
#define __cdecl
#define FFX_LINUX_COMPAT 1

#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifdef __cplusplus
#include <cmath>
#include <codecvt>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <locale>
#include <new>
#include <string>

inline int sprintf_s(char* dst, size_t size, const char* format, ...) {
  va_list args;
  va_start(args, format);
  int n = vsnprintf(dst, size, format, args);
  va_end(args);
  return n;
}
template <size_t N>
inline int sprintf_s(char (&dst)[N], const char* format, ...) {
  va_list args;
  va_start(args, format);
  int n = vsnprintf(dst, N, format, args);
  va_end(args);
  return n;
}

inline int wcscpy_s(wchar_t* dst, size_t size, const wchar_t* src) {
  if (!dst || size == 0) return EINVAL;
  size_t n = wcslen(src);
  if (n >= size) n = size - 1;
  wmemcpy(dst, src, n);
  dst[n] = L'\0';
  return 0;
}
template <size_t N>
inline int wcscpy_s(wchar_t (&dst)[N], const wchar_t* src) {
  return wcscpy_s(dst, N, src);
}
inline int strcpy_s(char* dst, size_t size, const char* src) {
  if (!dst || size == 0) return EINVAL;
  size_t n = strlen(src);
  if (n >= size) n = size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return 0;
}
template <size_t N>
inline int strcpy_s(char (&dst)[N], const char* src) {
  return strcpy_s(dst, N, src);
}
inline int strncpy_s(char* dst, size_t size, const char* src, size_t count) {
  if (!dst || size == 0) return EINVAL;
  size_t n = strnlen(src, count);
  if (n >= size) n = size - 1;
  memcpy(dst, src, n);
  dst[n] = '\0';
  return 0;
}
template <size_t N>
inline int strncpy_s(char (&dst)[N], const char* src, size_t count) {
  return strncpy_s(dst, N, src, count);
}
// The SDK's names are ASCII: a plain narrowing copy.
inline int wcstombs_s(size_t* converted, char* dst, size_t size, const wchar_t* src, size_t count) {
  size_t i = 0;
  if (dst && size) {
    for (; i + 1 < size && i < count && src[i]; ++i) dst[i] = char(src[i]);
    dst[i] = '\0';
  }
  if (converted) *converted = i + 1;
  return 0;
}
inline int mbstowcs_s(size_t* converted, wchar_t* dst, size_t size, const char* src, size_t count) {
  size_t i = 0;
  if (dst && size) {
    for (; i + 1 < size && i < count && src[i]; ++i) dst[i] = wchar_t((unsigned char)src[i]);
    dst[i] = L'\0';
  }
  if (converted) *converted = i + 1;
  return 0;
}
template <size_t N>
inline int mbstowcs_s(size_t* converted, wchar_t (&dst)[N], const char* src, size_t count) {
  return mbstowcs_s(converted, dst, N, src, count);
}
#endif  // __cplusplus

#endif  // !_WIN32
