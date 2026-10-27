#include "strbuf.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "memory.h"

// ensures room for at least capacity bytes plus the terminator.
static void
Reserve(StrBuf *buffer, u64 capacity)
{
  if (buffer->data && capacity < buffer->capacity) return;

  bool fresh = !buffer->data;
  u64 grown = buffer->capacity ? buffer->capacity * 2 : 64;
  while (grown <= capacity) grown *= 2;
  buffer->data = MemoryResize(buffer->data, grown, 1);
  buffer->capacity = grown;
  // a new buffer starts as the empty string, so every edit can count on the terminator.
  if (fresh) buffer->data[0] = '\0';
}

void
StrBufAppend(StrBuf *buffer, const char *data, u64 length)
{
  Reserve(buffer, buffer->length + length);
  if (length) memcpy(buffer->data + buffer->length, data, length);
  buffer->length += length;
  buffer->data[buffer->length] = '\0';
}

void
StrBufAppendString(StrBuf *buffer, const char *string)
{
  StrBufAppend(buffer, string, strlen(string));
}

void
StrBufAppendByte(StrBuf *buffer, char byte)
{
  StrBufAppend(buffer, &byte, 1);
}

void
StrBufAppendFormat(StrBuf *buffer, const char *format, ...)
{
  va_list arguments;
  va_start(arguments, format);
  va_list again;
  va_copy(again, arguments);
  int length = vsnprintf(nullptr, 0, format, arguments);
  va_end(arguments);

  if (length > 0) {
    Reserve(buffer, buffer->length + (u64)length);
    vsnprintf(buffer->data + buffer->length, (u64)length + 1, format, again);
    buffer->length += (u64)length;
  }
  va_end(again);
}

void
StrBufInsert(StrBuf *buffer, u64 at, const char *data, u64 length)
{
  Reserve(buffer, buffer->length + length);
  memmove(buffer->data + at + length, buffer->data + at, buffer->length - at + 1);
  memcpy(buffer->data + at, data, length);
  buffer->length += length;
}

void
StrBufRemove(StrBuf *buffer, u64 at, u64 length)
{
  if (!length) return;

  memmove(buffer->data + at, buffer->data + at + length, buffer->length - at - length + 1);
  buffer->length -= length;
}

void
StrBufClear(StrBuf *buffer)
{
  buffer->length = 0;
  if (buffer->data) buffer->data[0] = '\0';
}

void
StrBufFree(StrBuf *buffer)
{
  free(buffer->data);
  *buffer = (StrBuf){};
}

bool
StrBufReadFile(StrBuf *buffer, const char *path)
{
  StrBufClear(buffer);
  int file = open(path, O_RDONLY | O_CLOEXEC);
  if (file < 0) return false;

  while (true) {
    Reserve(buffer, buffer->length + 4096);
    ssize_t count = read(file, buffer->data + buffer->length, 4096);
    if (count <= 0) break;
    buffer->length += (u64)count;
  }
  buffer->data[buffer->length] = '\0';
  close(file);
  return true;
}
