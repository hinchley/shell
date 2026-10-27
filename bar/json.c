#include "json.h"

#include <stdlib.h>
#include <string.h>

static const char *
SkipSpace(const char *at)
{
  while (*at == ' ' || *at == '\t' || *at == '\n' || *at == '\r') at++;
  return at;
}

// the character after the string starting at at (its opening quote).
static const char *
SkipString(const char *at)
{
  for (at++; *at && *at != '"'; at++) {
    if (*at == '\\' && at[1]) at++;
  }
  return *at ? at + 1 : at;
}

// the character after the value starting at at.
static const char *
SkipValue(const char *at)
{
  at = SkipSpace(at);
  if (*at == '"') return SkipString(at);
  if (*at != '{' && *at != '[') {
    while (*at && *at != ',' && *at != '}' && *at != ']') at++;
    return at;
  }

  // an object or array: skip to its matching close, minding strings.
  u64 depth = 0;
  while (*at) {
    if (*at == '"') {
      at = SkipString(at);
      continue;
    }
    if (*at == '{' || *at == '[') depth++;
    else if ((*at == '}' || *at == ']') && --depth == 0) return at + 1;
    at++;
  }
  return at;
}

JsonArray
JsonArrayBegin(const char *value)
{
  return (JsonArray){.at = value ? SkipSpace(value) : nullptr};
}

const char *
JsonArrayNext(JsonArray *array)
{
  if (!array->at) return nullptr;

  const char *at = array->at;
  if (!array->started) {
    if (*at != '[') return nullptr;
    array->started = true;
    at++;
  } else {
    at = SkipSpace(at);
    if (*at != ',') return nullptr;
    at++;
  }

  at = SkipSpace(at);
  if (!*at || *at == ']') {
    array->at = nullptr;
    return nullptr;
  }
  array->at = SkipValue(at);
  return at;
}

const char *
JsonMember(const char *object, const char *key)
{
  if (!object) return nullptr;
  const char *at = SkipSpace(object);
  if (*at != '{') return nullptr;

  u64 length = strlen(key);
  at = SkipSpace(at + 1);
  while (*at == '"') {
    const char *name = at + 1;
    const char *end = SkipString(at);
    // a key with escapes in it never matches, which hyprland's keys never need.
    bool matches = (u64)(end - 1 - name) == length && strncmp(name, key, length) == 0;

    at = SkipSpace(end);
    if (*at != ':') return nullptr;
    at = SkipSpace(at + 1);
    if (matches) return at;

    at = SkipSpace(SkipValue(at));
    if (*at != ',') return nullptr;
    at = SkipSpace(at + 1);
  }
  return nullptr;
}

i64
JsonInteger(const char *value, i64 fallback)
{
  if (!value) return fallback;

  char *end;
  i64 number = strtoll(value, &end, 10);
  return end == value ? fallback : number;
}

// appends codepoint as utf-8.
static void
AppendUtf8(StrBuf *out, u32 codepoint)
{
  if (codepoint < 0x80) {
    StrBufAppendByte(out, (char)codepoint);
  } else if (codepoint < 0x800) {
    StrBufAppendByte(out, (char)(0xC0 | codepoint >> 6));
    StrBufAppendByte(out, (char)(0x80 | (codepoint & 0x3F)));
  } else if (codepoint < 0x10000) {
    StrBufAppendByte(out, (char)(0xE0 | codepoint >> 12));
    StrBufAppendByte(out, (char)(0x80 | (codepoint >> 6 & 0x3F)));
    StrBufAppendByte(out, (char)(0x80 | (codepoint & 0x3F)));
  } else {
    StrBufAppendByte(out, (char)(0xF0 | codepoint >> 18));
    StrBufAppendByte(out, (char)(0x80 | (codepoint >> 12 & 0x3F)));
    StrBufAppendByte(out, (char)(0x80 | (codepoint >> 6 & 0x3F)));
    StrBufAppendByte(out, (char)(0x80 | (codepoint & 0x3F)));
  }
}

// the four hex digits at at as a number.
static u32
Hex4(const char *at)
{
  u32 value = 0;
  for (u32 index = 0; index < 4; index++) {
    char digit = at[index];
    value <<= 4;
    if (digit >= '0' && digit <= '9') value |= (u32)(digit - '0');
    else if (digit >= 'a' && digit <= 'f') value |= (u32)(digit - 'a' + 10);
    else if (digit >= 'A' && digit <= 'F') value |= (u32)(digit - 'A' + 10);
  }
  return value;
}

bool
JsonString(const char *value, StrBuf *out)
{
  StrBufClear(out);
  if (!value || *value != '"') return false;

  for (const char *at = value + 1; *at && *at != '"'; at++) {
    if (*at != '\\') {
      StrBufAppendByte(out, *at);
      continue;
    }

    at++;
    switch (*at) {
      case 'n':
        StrBufAppendByte(out, '\n');
        break;
      case 't':
        StrBufAppendByte(out, '\t');
        break;
      case 'r':
        StrBufAppendByte(out, '\r');
        break;
      case 'b':
        StrBufAppendByte(out, '\b');
        break;
      case 'f':
        StrBufAppendByte(out, '\f');
        break;
      case 'u': {
        if (strnlen(at + 1, 4) < 4) return true;
        u32 codepoint = Hex4(at + 1);
        at += 4;
        // a surrogate pair is two escapes making one codepoint.
        if (codepoint >= 0xD800 && codepoint < 0xDC00 && at[1] == '\\' && at[2] == 'u' &&
          strnlen(at + 3, 4) == 4) {
          u32 low = Hex4(at + 3);
          codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
          at += 6;
        }
        AppendUtf8(out, codepoint);
        break;
      }
      case '\0':
        return true;
      default:
        StrBufAppendByte(out, *at);
        break;
    }
  }
  return true;
}
