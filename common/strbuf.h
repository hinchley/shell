#pragma once

// growable byte string.
// a zeroed StrBuf is a valid empty string.
// once memory is allocated, data is always nul terminated so it can be passed to c apis.
#include "types.h"

typedef struct StrBuf {
  char *data;
  u64 length;
  u64 capacity;
} StrBuf;

void
StrBufAppend(StrBuf *buffer, const char *data, u64 length);

void
StrBufAppendString(StrBuf *buffer, const char *string);

void
StrBufAppendByte(StrBuf *buffer, char byte);

[[gnu::format(printf, 2, 3)]] void
StrBufAppendFormat(StrBuf *buffer, const char *format, ...);

void
StrBufInsert(StrBuf *buffer, u64 at, const char *data, u64 length);

void
StrBufRemove(StrBuf *buffer, u64 at, u64 length);

// empties the string but keeps its memory.
void
StrBufClear(StrBuf *buffer);

void
StrBufFree(StrBuf *buffer);

// text's bytes as a c string, empty when nothing was ever allocated.
static inline const char *
StrBufText(const StrBuf *text)
{
  return text->data ? text->data : "";
}

// replaces buffer's contents with the whole file at path.
// false (with buffer empty) when it cannot be read.
bool
StrBufReadFile(StrBuf *buffer, const char *path);
