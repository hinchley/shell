#include "memory.h"

#include <errno.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void
Fatal(const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  fprintf(stderr, "%s: ", program_invocation_short_name);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
  exit(1);
}

void
Warn(const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  fprintf(stderr, "%s: ", program_invocation_short_name);
  vfprintf(stderr, format, arguments);
  fputc('\n', stderr);
  va_end(arguments);
}

void *
MemoryAllocateZeroed(u64 count, u64 size)
{
  void *pointer = calloc(count ? count : 1, size ? size : 1);
  if (!pointer) Fatal("out of memory allocating %lu x %lu bytes", count, size);
  return pointer;
}

void *
MemoryResize(void *pointer, u64 count, u64 size)
{
  u64 total;
  if (ckd_mul(&total, count, size)) Fatal("allocation size overflow (%lu x %lu)", count, size);

  void *resized = realloc(pointer, total ? total : 1);
  if (!resized) Fatal("out of memory resizing to %lu bytes", total);
  return resized;
}

char *
MemoryCopyString(const char *string)
{
  return MemoryCopyStringLength(string, strlen(string));
}

char *
MemoryCopyStringLength(const char *string, u64 length)
{
  char *copy = MemoryResize(nullptr, length + 1, 1);
  memcpy(copy, string, length);
  copy[length] = '\0';
  return copy;
}

void
MemoryTrim(void)
{
  malloc_trim(0);
}
